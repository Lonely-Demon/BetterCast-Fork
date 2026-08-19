package com.bettercast.receiver.network

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import java.io.ByteArrayOutputStream
import java.math.BigInteger
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.AlgorithmParameters
import java.security.KeyFactory
import java.security.KeyPair
import java.security.KeyPairGenerator
import java.security.KeyStore
import java.security.MessageDigest
import java.security.PrivateKey
import java.security.PublicKey
import java.security.SecureRandom
import java.security.Signature
import java.security.spec.ECGenParameterSpec
import java.security.spec.ECParameterSpec
import java.security.spec.ECPoint
import java.security.spec.ECPublicKeySpec
import javax.crypto.Cipher
import javax.crypto.KeyAgreement
import javax.crypto.Mac
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

/**
 * BetterCast protocol v2 secure session.
 *
 * The class deliberately fails closed: plaintext frames are not accepted by this
 * session and record processing is available only after both transcript
 * authentication and key confirmation have completed.
 */
class SecureSession(
    private val role: Role,
    private val context: Context,
) {
    enum class Role(val wire: Int) { INITIATOR(1), RESPONDER(2) }
    enum class State { NEW, HELLO_EXCHANGED, HANDSHAKE_CONFIRMED, AWAITING_APPROVAL, ESTABLISHED, FAILED }

    companion object {
        const val PROTOCOL_VERSION = 2
        const val SUITE_AES_256_GCM_P256 = 1
        const val MAX_HANDSHAKE_MESSAGE = 512
        const val MAX_RECORD_PLAINTEXT = 8 * 1024 * 1024
        private const val IDENTITY_ALIAS = "bettercast.identity.v2"
        private const val IDENTITY_PREFS = "bettercast_secure_identity"
        private const val MAGIC = 0x4243
        private const val HELLO = 0x01
        private const val HELLO_REPLY = 0x02
        private const val AUTHENTICATION = 0x03
        private const val CONFIRMATION = 0x04
        private const val P256_BYTES = 32
        private const val PUBLIC_KEY_BYTES = 65
        private const val NONCE_BYTES = 32
        private const val GCM_NONCE_BYTES = 12
        private const val GCM_TAG_BYTES = 16
        private const val AUTH_TAG_BYTES = 32
        private const val MAX_ECDSA_DER_SIGNATURE = 80

        private fun u32(value: Int): ByteArray = ByteBuffer.allocate(4).order(ByteOrder.BIG_ENDIAN).putInt(value).array()
        private fun u64(value: Long): ByteArray = ByteBuffer.allocate(8).order(ByteOrder.BIG_ENDIAN).putLong(value).array()
        private fun readU32(bytes: ByteArray, offset: Int): Int = ByteBuffer.wrap(bytes, offset, 4).order(ByteOrder.BIG_ENDIAN).int
        private fun readU64(bytes: ByteArray, offset: Int): Long = ByteBuffer.wrap(bytes, offset, 8).order(ByteOrder.BIG_ENDIAN).long

        private fun fixed32(value: BigInteger): ByteArray {
            val source = value.toByteArray()
            val unsigned = if (source.size > 1 && source[0] == 0.toByte()) source.copyOfRange(1, source.size) else source
            require(unsigned.size <= P256_BYTES)
            return ByteArray(P256_BYTES - unsigned.size) + unsigned
        }

        private fun publicKeyBytes(publicKey: PublicKey): ByteArray {
            val ec = publicKey as java.security.interfaces.ECPublicKey
            return byteArrayOf(0x04) + fixed32(ec.w.affineX) + fixed32(ec.w.affineY)
        }

        private fun ecParameters(): ECParameterSpec {
            val parameters = AlgorithmParameters.getInstance("EC")
            parameters.init(ECGenParameterSpec("secp256r1"))
            return parameters.getParameterSpec(ECParameterSpec::class.java)
        }

        private fun decodePublicKey(encoded: ByteArray): PublicKey {
            require(encoded.size == PUBLIC_KEY_BYTES && encoded[0].toInt() and 0xff == 0x04)
            val x = BigInteger(1, encoded.copyOfRange(1, 33))
            val y = BigInteger(1, encoded.copyOfRange(33, 65))
            return KeyFactory.getInstance("EC").generatePublic(ECPublicKeySpec(ECPoint(x, y), ecParameters()))
        }
    }

    private var _state = State.NEW
    val state: State get() = _state
    private var identityPrivateKey: PrivateKey? = null
    private var identityPublicKey: ByteArray = ByteArray(0)
    private var localNonce = ByteArray(0)
    private var peerNonce = ByteArray(0)
    private var localEphemeral: KeyPair? = null
    private var localEphemeralPublic = ByteArray(0)
    private var peerEphemeralPublic = ByteArray(0)
    private var peerIdentityPublic = ByteArray(0)
    private var localHello = ByteArray(0)
    private var peerHello = ByteArray(0)
    private var transcript = ByteArray(0)
    private var peerFingerprintBytes = ByteArray(0)
    private var shortAuthBytes = ByteArray(0)
    private var txKey = ByteArray(0)
    private var rxKey = ByteArray(0)
    private var txNonceBase = ByteArray(0)
    private var rxNonceBase = ByteArray(0)
    private var confirmInitiator = ByteArray(0)
    private var confirmResponder = ByteArray(0)
    private var txSequence = 1L
    private var rxSequence = 1L

    fun loadOrCreateIdentity(): Boolean {
        return try {
            val keyStore = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
            if (!keyStore.containsAlias(IDENTITY_ALIAS)) {
                val generator = KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_EC, "AndroidKeyStore")
                generator.initialize(
                    KeyGenParameterSpec.Builder(
                        IDENTITY_ALIAS,
                        KeyProperties.PURPOSE_SIGN or KeyProperties.PURPOSE_VERIFY,
                    ).setAlgorithmParameterSpec(ECGenParameterSpec("secp256r1"))
                        .setDigests(KeyProperties.DIGEST_SHA256)
                        .build()
                )
                generator.generateKeyPair()
            }
            val entry = keyStore.getEntry(IDENTITY_ALIAS, null) as KeyStore.PrivateKeyEntry
            identityPrivateKey = entry.privateKey
            identityPublicKey = publicKeyBytes(entry.certificate.publicKey)
            true
        } catch (_: Exception) {
            _state = State.FAILED
            false
        }
    }

    private fun fail(message: String): Boolean {
        _state = State.FAILED
        return false
    }

    private fun buildHello(type: Int, nonce: ByteArray, ephemeral: ByteArray, identity: ByteArray): ByteArray {
        return byteArrayOf('B'.code.toByte(), 'C'.code.toByte(), PROTOCOL_VERSION.toByte(), type.toByte(), role.wire.toByte(), SUITE_AES_256_GCM_P256.toByte()) + nonce + ephemeral + identity
    }

    private fun validHello(message: ByteArray, expectedType: Int): Boolean {
        return message.size == 2 + 4 + NONCE_BYTES + PUBLIC_KEY_BYTES + PUBLIC_KEY_BYTES &&
            message[0] == 'B'.code.toByte() && message[1] == 'C'.code.toByte() &&
            message[2].toInt() and 0xff == PROTOCOL_VERSION && message[3].toInt() and 0xff == expectedType &&
            message[5].toInt() and 0xff == SUITE_AES_256_GCM_P256 &&
            runCatching { decodePublicKey(message.copyOfRange(6 + NONCE_BYTES, 6 + NONCE_BYTES + PUBLIC_KEY_BYTES)); true }.getOrDefault(false) &&
            runCatching { decodePublicKey(message.copyOfRange(6 + NONCE_BYTES + PUBLIC_KEY_BYTES, message.size)); true }.getOrDefault(false)
    }

    fun makeHello(): ByteArray? {
        if (_state != State.NEW || identityPrivateKey == null) return null
        return try {
            val generator = KeyPairGenerator.getInstance("EC")
            generator.initialize(ECGenParameterSpec("secp256r1"))
            localEphemeral = generator.generateKeyPair()
            localEphemeralPublic = publicKeyBytes(localEphemeral!!.public)
            localNonce = ByteArray(NONCE_BYTES).also { SecureRandom().nextBytes(it) }
            localHello = buildHello(if (role == Role.INITIATOR) HELLO else HELLO_REPLY, localNonce, localEphemeralPublic, identityPublicKey)
            _state = State.HELLO_EXCHANGED
            localHello
        } catch (_: Exception) {
            fail("Unable to create secure hello")
            null
        }
    }

    fun receiveHello(message: ByteArray): ByteArray? {
        if (role != Role.RESPONDER || _state != State.NEW || !validHello(message, HELLO)) {
            fail("Invalid secure hello")
            return null
        }
        if (message[4].toInt() and 0xff != Role.INITIATOR.wire) {
            fail("Invalid secure hello role")
            return null
        }
        return try {
            peerHello = message.copyOf()
            peerNonce = message.copyOfRange(6, 6 + NONCE_BYTES)
            peerEphemeralPublic = message.copyOfRange(6 + NONCE_BYTES, 6 + NONCE_BYTES + PUBLIC_KEY_BYTES)
            peerIdentityPublic = message.copyOfRange(6 + NONCE_BYTES + PUBLIC_KEY_BYTES, message.size)
            val generator = KeyPairGenerator.getInstance("EC")
            generator.initialize(ECGenParameterSpec("secp256r1"))
            localEphemeral = generator.generateKeyPair()
            localEphemeralPublic = publicKeyBytes(localEphemeral!!.public)
            localNonce = ByteArray(NONCE_BYTES).also { SecureRandom().nextBytes(it) }
            localHello = buildHello(HELLO_REPLY, localNonce, localEphemeralPublic, identityPublicKey)
            _state = State.HELLO_EXCHANGED
            localHello
        } catch (_: Exception) {
            fail("Unable to process secure hello")
            null
        }
    }

    fun receiveHelloReply(message: ByteArray): ByteArray? {
        if (role != Role.INITIATOR || _state != State.HELLO_EXCHANGED || !validHello(message, HELLO_REPLY)) {
            fail("Invalid secure hello reply")
            return null
        }
        if (message[4].toInt() and 0xff != Role.RESPONDER.wire) {
            fail("Invalid secure hello reply role")
            return null
        }
        peerHello = message.copyOf()
        peerNonce = message.copyOfRange(6, 6 + NONCE_BYTES)
        peerEphemeralPublic = message.copyOfRange(6 + NONCE_BYTES, 6 + NONCE_BYTES + PUBLIC_KEY_BYTES)
        peerIdentityPublic = message.copyOfRange(6 + NONCE_BYTES + PUBLIC_KEY_BYTES, message.size)
        return if (deriveKeys()) makeAuthentication() else null
    }

    private fun sha256(input: ByteArray): ByteArray = MessageDigest.getInstance("SHA-256").digest(input)

    private fun hmac(key: ByteArray, input: ByteArray): ByteArray {
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(key, "HmacSHA256"))
        return mac.doFinal(input)
    }

    private fun hkdfExpand(prk: ByteArray, info: ByteArray, length: Int): ByteArray {
        require(length in 0..(255 * 32))
        val result = ByteArrayOutputStream()
        var previous = ByteArray(0)
        var counter = 1
        while (result.size() < length) {
            previous = hmac(prk, previous + info + byteArrayOf(counter.toByte()))
            result.write(previous)
            counter++
        }
        return result.toByteArray().copyOf(length)
    }

    private fun deriveKeys(): Boolean {
        return try {
            val peerKey = decodePublicKey(peerEphemeralPublic)
            val agreement = KeyAgreement.getInstance("ECDH")
            agreement.init(localEphemeral!!.private)
            agreement.doPhase(peerKey, true)
            val shared = agreement.generateSecret()
            val initiatorNonce = if (role == Role.INITIATOR) localNonce else peerNonce
            val responderNonce = if (role == Role.INITIATOR) peerNonce else localNonce
            transcript = if (role == Role.INITIATOR) localHello + peerHello else peerHello + localHello
            val transcriptHash = sha256(transcript)
            val salt = sha256("BetterCast/v2/salt".toByteArray() + initiatorNonce + responderNonce)
            val prk = hmac(salt, shared)
            fun expand(label: String, length: Int) = hkdfExpand(prk, "BetterCast/v2/".toByteArray() + label.toByteArray() + transcriptHash, length)
            val initiatorTx = expand("initiator-tx", 32)
            val responderTx = expand("responder-tx", 32)
            val initiatorNonceBase = expand("initiator-nonce", GCM_NONCE_BYTES)
            val responderNonceBase = expand("responder-nonce", GCM_NONCE_BYTES)
            confirmInitiator = expand("confirm-initiator", 32)
            confirmResponder = expand("confirm-responder", 32)
            if (role == Role.INITIATOR) {
                txKey = initiatorTx; rxKey = responderTx
                txNonceBase = initiatorNonceBase; rxNonceBase = responderNonceBase
            } else {
                txKey = responderTx; rxKey = initiatorTx
                txNonceBase = responderNonceBase; rxNonceBase = initiatorNonceBase
            }
            if (pinnedPeerPublicKey.isNotEmpty() && !pinnedPeerPublicKey.contentEquals(peerIdentityPublic)) {
                return fail("Pinned peer identity changed")
            }
            peerFingerprintBytes = sha256(peerIdentityPublic)
            shortAuthBytes = sha256("BetterCast/v2/sas".toByteArray() + transcript).copyOf(6)
            true
        } catch (_: Exception) {
            fail("Secure key derivation failed")
            false
        }
    }

    private fun signTranscript(): ByteArray? {
        return try {
            val signature = Signature.getInstance("SHA256withECDSA")
            signature.initSign(identityPrivateKey!!)
            signature.update(transcript)
            signature.sign()
        } catch (_: Exception) { null }
    }

    private fun verifyTranscript(signatureBytes: ByteArray): Boolean {
        return try {
            val signature = Signature.getInstance("SHA256withECDSA")
            signature.initVerify(decodePublicKey(peerIdentityPublic))
            signature.update(transcript)
            signature.verify(signatureBytes)
        } catch (_: Exception) { false }
    }

    fun makeAuthentication(): ByteArray? {
        if (_state != State.HELLO_EXCHANGED && _state != State.HANDSHAKE_CONFIRMED) return null
        if (transcript.isEmpty() && !deriveKeys()) return null
        val signature = signTranscript() ?: return null
        return byteArrayOf(AUTHENTICATION.toByte()) + u32(signature.size) + signature
    }

    fun receiveAuthentication(message: ByteArray): ByteArray? {
        if (message.size < 13 || message.size > 1 + 4 + MAX_ECDSA_DER_SIGNATURE || message[0].toInt() and 0xff != AUTHENTICATION) {
            fail("Invalid secure authentication message")
            return null
        }
        if (transcript.isEmpty() && !deriveKeys()) return null
        val length = readU32(message, 1)
        if (length !in 8..MAX_ECDSA_DER_SIGNATURE || message.size != 5 + length) {
            fail("Invalid secure authentication signature length")
            return null
        }
        if (!verifyTranscript(message.copyOfRange(5, 5 + length))) {
            fail("Secure authentication failed")
            return null
        }
        val response = if (role == Role.RESPONDER) makeAuthentication() else null
        _state = State.HANDSHAKE_CONFIRMED
        return response
    }

    private fun confirmation(initiator: Boolean): ByteArray {
        val key = if (initiator) confirmInitiator else confirmResponder
        return hmac(key, "BetterCast/v2/confirm".toByteArray() + transcript)
    }

    fun makeConfirmation(initiator: Boolean = role == Role.INITIATOR): ByteArray? {
        if (_state != State.HANDSHAKE_CONFIRMED && _state != State.AWAITING_APPROVAL) return null
        return byteArrayOf(CONFIRMATION.toByte()) + confirmation(initiator)
    }

    fun receiveConfirmation(message: ByteArray): ByteArray? {
        if (message.size != 1 + AUTH_TAG_BYTES || message[0].toInt() and 0xff != CONFIRMATION) {
            fail("Invalid secure confirmation")
            return null
        }
        val expected = confirmation(role == Role.RESPONDER)
        if (!MessageDigest.isEqual(expected, message.copyOfRange(1, message.size))) {
            fail("Secure key confirmation failed")
            return null
        }
        val response = if (role == Role.RESPONDER) makeConfirmation(false) else null
        _state = State.AWAITING_APPROVAL
        return response
    }

    private var pinnedPeerPublicKey: ByteArray = ByteArray(0)

    fun setPinnedPeerPublicKey(publicKey: ByteArray) {
        if (publicKey.size == PUBLIC_KEY_BYTES) pinnedPeerPublicKey = publicKey.copyOf()
    }

    fun hasPinnedPeer(): Boolean = pinnedPeerPublicKey.isNotEmpty()

    fun peerIdentityPublicKey(): ByteArray = peerIdentityPublic.copyOf()

    fun approvePeer(): Boolean {
        if (_state != State.AWAITING_APPROVAL && _state != State.HANDSHAKE_CONFIRMED) return false
        if (peerIdentityPublic.size != PUBLIC_KEY_BYTES) return false
        pinnedPeerPublicKey = peerIdentityPublic.copyOf()
        _state = State.ESTABLISHED
        return true
    }

    fun peerFingerprint(): String = peerFingerprintBytes.joinToString("") { "%02X".format(it) }

    fun shortAuthenticationString(): String {
        val hex = shortAuthBytes.joinToString("") { "%02X".format(it) }
        return if (hex.length == 12) "${hex.substring(0, 4)}-${hex.substring(4, 8)}-${hex.substring(8, 12)}" else ""
    }

    private fun nonceForSequence(base: ByteArray, sequence: Long): ByteArray {
        val nonce = base.copyOf()
        val counter = u64(sequence)
        for (i in 0 until 8) nonce[4 + i] = (nonce[4 + i].toInt() xor counter[i].toInt()).toByte()
        return nonce
    }

    fun encryptRecord(type: Int, plaintext: ByteArray): ByteArray? {
        if (_state != State.ESTABLISHED || type !in 1..255 || plaintext.size > MAX_RECORD_PLAINTEXT || txSequence <= 0) return null
        val sequence = txSequence++
        val header = byteArrayOf(
            (MAGIC shr 8).toByte(), MAGIC.toByte(), PROTOCOL_VERSION.toByte(),
            (if (role == Role.INITIATOR) 1 else 2).toByte(), type.toByte(), 0,
        ) + u64(sequence) + u32(plaintext.size)
        return try {
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.ENCRYPT_MODE, SecretKeySpec(txKey, "AES"), GCMParameterSpec(128, nonceForSequence(txNonceBase, sequence)))
            cipher.updateAAD(header)
            header + cipher.doFinal(plaintext)
        } catch (_: Exception) { null }
    }

    fun decryptRecord(record: ByteArray): Pair<Int, ByteArray>? {
        val headerSize = 18
        if (_state != State.ESTABLISHED || record.size < headerSize + GCM_TAG_BYTES) return null
        val magic = ((record[0].toInt() and 0xff) shl 8) or (record[1].toInt() and 0xff)
        val version = record[2].toInt() and 0xff
        val direction = record[3].toInt() and 0xff
        val type = record[4].toInt() and 0xff
        val sequence = readU64(record, 6)
        val length = readU32(record, 14)
        val expectedDirection = if (role == Role.INITIATOR) 2 else 1
        if (magic != MAGIC || version != PROTOCOL_VERSION || direction != expectedDirection || type == 0 ||
            length !in 0..MAX_RECORD_PLAINTEXT || record.size != headerSize + length + GCM_TAG_BYTES || sequence != rxSequence) return null
        return try {
            val header = record.copyOfRange(0, headerSize)
            val ciphertextAndTag = record.copyOfRange(headerSize, record.size)
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.DECRYPT_MODE, SecretKeySpec(rxKey, "AES"), GCMParameterSpec(128, nonceForSequence(rxNonceBase, sequence)))
            cipher.updateAAD(header)
            val plaintext = cipher.doFinal(ciphertextAndTag)
            rxSequence++
            type to plaintext
        } catch (_: Exception) { null }
    }
}

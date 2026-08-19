package com.bettercast.receiver.network

import android.content.Context
import android.util.Base64
import android.util.Log
import com.bettercast.receiver.input.InputEvent
import kotlinx.coroutines.*
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.IOException
import java.net.ServerSocket
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder

enum class ConnectionState { IDLE, LISTENING, CONNECTED, ERROR, PAIRING }

/** Secure Android receiver TCP endpoint. Plaintext media/control frames are rejected. */
class TcpClient(private val context: Context) {

    companion object {
        private const val TAG = "TcpServer"
        private const val HEARTBEAT_INTERVAL_MS = 500L
        private const val DEFAULT_PORT = 51820
        private const val MAX_HANDSHAKE_MESSAGE = 512
        private const val MAX_FRAME_SIZE = 8 * 1024 * 1024 + 1024
        private const val CONTROL_TYPE = 0x03
        private const val MAX_CONTROL_BYTES = 16 * 1024
        private const val MAX_CONTROL_PER_SECOND = 240
        private const val CONTROL_QUEUE_CAPACITY = 32
        private const val PREFS = "bettercast_secure_transport"
        private const val PEER_KEY = "windows_peer_public_key"
    }

    private val _connectionState = MutableStateFlow(ConnectionState.IDLE)
    val connectionState: StateFlow<ConnectionState> = _connectionState.asStateFlow()
    private val _errorMessage = MutableStateFlow<String?>(null)
    val errorMessage: StateFlow<String?> = _errorMessage.asStateFlow()
    private val _connectedSenderName = MutableStateFlow<String?>(null)
    val connectedSenderName: StateFlow<String?> = _connectedSenderName.asStateFlow()

    private var serverSocket: ServerSocket? = null
    private var clientSocket: Socket? = null
    private var outputStream: DataOutputStream? = null
    private var inputStream: DataInputStream? = null
    private var secureSession: SecureSession? = null
    private var pendingSession: SecureSession? = null
    private var pendingPairingJob: Job? = null
    private var pendingPairingSocket: Socket? = null

    private val sendQueue = Channel<ByteArray>(CONTROL_QUEUE_CAPACITY)
    private var acceptJob: Job? = null
    private var readJob: Job? = null
    private var writeJob: Job? = null
    private var heartbeatJob: Job? = null
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    var onFrameReceived: ((ByteArray) -> Unit)? = null
    var onAudioReceived: ((ByteArray) -> Unit)? = null
    var onControlReceived: ((ByteArray) -> Unit)? = null
    var onPairingRequired: ((authenticationString: String, fingerprint: String) -> Unit)? = null

    var listeningPort: Int = 0
        private set

    fun startListening(): Int {
        if (_connectionState.value == ConnectionState.LISTENING ||
            _connectionState.value == ConnectionState.CONNECTED ||
            _connectionState.value == ConnectionState.PAIRING) return listeningPort
        _errorMessage.value = null
        return try {
            val server = ServerSocket(DEFAULT_PORT)
            serverSocket = server
            listeningPort = server.localPort
            _connectionState.value = ConnectionState.LISTENING
            Log.d(TAG, "Listening securely on port $listeningPort")
            startAcceptLoop(server)
            listeningPort
        } catch (e: Exception) {
            Log.e(TAG, "Failed to start server", e)
            _connectionState.value = ConnectionState.ERROR
            _errorMessage.value = "Failed to start secure listener: ${e.message}"
            0
        }
    }

    private fun readBounded(input: DataInputStream, max: Int): ByteArray? {
        val length = try { input.readInt() } catch (_: IOException) { return null }
        if (length <= 0 || length > max) return null
        return try { ByteArray(length).also { input.readFully(it) } } catch (_: IOException) { null }
    }

    private fun writeFramed(output: DataOutputStream, data: ByteArray): Boolean {
        if (data.isEmpty() || data.size > MAX_FRAME_SIZE) return false
        return try {
            output.writeInt(data.size)
            output.write(data)
            output.flush()
            true
        } catch (_: IOException) { false }
    }

    private fun loadPinnedPeer(session: SecureSession) {
        val encoded = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(PEER_KEY, null) ?: return
        runCatching { Base64.decode(encoded, Base64.DEFAULT) }.getOrNull()?.let(session::setPinnedPeerPublicKey)
    }

    private fun persistPeer(session: SecureSession) {
        val publicKey = session.peerIdentityPublicKey()
        if (publicKey.isEmpty()) return
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
            .putString(PEER_KEY, Base64.encodeToString(publicKey, Base64.NO_WRAP)).apply()
    }

    private fun responderHandshake(socket: Socket, input: DataInputStream, output: DataOutputStream): SecureSession? {
        val session = SecureSession(SecureSession.Role.RESPONDER, context)
        if (!session.loadOrCreateIdentity()) return null
        loadPinnedPeer(session)
        val hello = readBounded(input, MAX_HANDSHAKE_MESSAGE) ?: return null
        val reply = session.receiveHello(hello) ?: return null
        if (!writeFramed(output, reply)) return null
        val authentication = readBounded(input, MAX_HANDSHAKE_MESSAGE) ?: return null
        val responseAuthentication = session.receiveAuthentication(authentication) ?: return null
        if (!writeFramed(output, responseAuthentication)) return null
        val confirmation = readBounded(input, MAX_HANDSHAKE_MESSAGE) ?: return null
        val responseConfirmation = session.receiveConfirmation(confirmation) ?: return null
        if (!writeFramed(output, responseConfirmation)) return null
        if (session.hasPinnedPeer() && !session.approvePeer()) return null
        return session
    }

    private fun startAcceptLoop(server: ServerSocket) {
        acceptJob = scope.launch {
            try {
                while (isActive) {
                    val socket = server.accept()
                    if (_connectionState.value == ConnectionState.PAIRING) {
                        socket.close()
                        continue
                    }
                    socket.tcpNoDelay = true
                    socket.keepAlive = true
                    socket.soTimeout = 15_000
                    socket.receiveBufferSize = 524288
                    socket.sendBufferSize = 65536
                    disconnectClient()
                    val input = DataInputStream(socket.getInputStream())
                    val output = DataOutputStream(socket.getOutputStream())
                    val session = responderHandshake(socket, input, output)
                    if (session == null) {
                        socket.close()
                        continue
                    }
                    clientSocket = socket
                    inputStream = input
                    outputStream = output
                    secureSession = session
                    if (session.isEstablished()) {
                        activateSecureSession(socket)
                    } else {
                        pendingSession = session
                        pendingPairingSocket = socket
                        _connectionState.value = ConnectionState.PAIRING
                        _errorMessage.value = "Approve pairing ${session.shortAuthenticationString()} on both devices"
                        onPairingRequired?.invoke(session.shortAuthenticationString(), session.peerFingerprint())
                        pendingPairingJob?.cancel()
                        pendingPairingJob = launch {
                            delay(60_000)
                            if (pendingSession === session) handleClientDisconnect("Pairing approval timed out")
                        }
                    }
                }
            } catch (e: IOException) {
                if (isActive) Log.e(TAG, "Accept error", e)
            }
        }
    }

    private fun activateSecureSession(socket: Socket) {
        pendingPairingJob?.cancel()
        pendingPairingJob = null
        pendingSession = null
        pendingPairingSocket = null
        socket.soTimeout = 0
        _connectedSenderName.value = socket.remoteSocketAddress.toString()
        _connectionState.value = ConnectionState.CONNECTED
        _errorMessage.value = null
        startReadLoop()
        startWriteLoop()
        startHeartbeat()
    }

    fun approvePendingPairing(): Boolean {
        val session = pendingSession ?: return false
        if (!session.approvePeer()) return false
        persistPeer(session)
        val socket = pendingPairingSocket ?: return false
        activateSecureSession(socket)
        return true
    }

    private fun startReadLoop() {
        readJob?.cancel()
        readJob = scope.launch {
            val input = inputStream ?: return@launch
            val session = secureSession ?: return@launch
            var frameCount = 0L
            var audioCount = 0L
            var controlWindowStartMs = android.os.SystemClock.elapsedRealtime()
            var controlCountInWindow = 0
            try {
                while (isActive && session.isEstablished()) {
                    val record = readBounded(input, MAX_FRAME_SIZE) ?: throw IOException("Invalid or truncated secure record")
                    val decoded = session.decryptRecord(record) ?: throw IOException("Secure record authentication or sequence failure")
                    val type = decoded.first
                    val payload = decoded.second
                    when (type) {
                        0x01 -> {
                            frameCount++
                            if (frameCount <= 5 || frameCount % 300 == 0L) Log.i(TAG, "Secure video frame #$frameCount: ${payload.size} bytes")
                            onFrameReceived?.invoke(payload)
                        }
                        0x02 -> {
                            audioCount++
                            if (audioCount <= 3 || audioCount % 200 == 0L) Log.i(TAG, "Secure audio packet #$audioCount: ${payload.size} bytes")
                            onAudioReceived?.invoke(payload)
                        }
                        CONTROL_TYPE -> {
                            val now = android.os.SystemClock.elapsedRealtime()
                            if (now - controlWindowStartMs >= 1_000L) {
                                controlWindowStartMs = now
                                controlCountInWindow = 0
                            }
                            controlCountInWindow++
                            if (controlCountInWindow > MAX_CONTROL_PER_SECOND || payload.size > MAX_CONTROL_BYTES) {
                                throw IOException("Secure control rate or size limit exceeded")
                            }
                            onControlReceived?.invoke(payload)
                        }
                        else -> throw IOException("Unknown secure record type")
                    }
                }
            } catch (e: IOException) {
                if (isActive) {
                    Log.e(TAG, "Secure read error after $frameCount frames", e)
                    handleClientDisconnect("Secure session closed: ${e.message}")
                }
            }
        }
    }

    private fun startWriteLoop() {
        writeJob?.cancel()
        writeJob = scope.launch {
            val output = outputStream ?: return@launch
            try {
                for (data in sendQueue) {
                    if (!isActive) break
                    if (!writeFramed(output, data)) throw IOException("Secure write failed")
                }
            } catch (e: IOException) {
                if (isActive) handleClientDisconnect("Secure write error: ${e.message}")
            }
        }
    }

    private fun startHeartbeat() {
        heartbeatJob?.cancel()
        heartbeatJob = scope.launch {
            while (isActive && _connectionState.value == ConnectionState.CONNECTED) {
                delay(HEARTBEAT_INTERVAL_MS)
                sendInputEvent(InputEvent.heartbeat())
            }
        }
    }

    fun sendInputEvent(event: InputEvent) {
        val session = secureSession ?: return
        if (!session.isEstablished()) return
        val jsonBytes = Json.encodeToString(event).toByteArray(Charsets.UTF_8)
        if (jsonBytes.isEmpty() || jsonBytes.size > MAX_CONTROL_BYTES) return
        val repeatCount = if (InputEvent.isCritical(event.type)) 3 else 1
        repeat(repeatCount) {
            // Each retry must receive a fresh sequence number and nonce; reusing
            // the same ciphertext would be rejected as a replay by the peer.
            val record = session.encryptRecord(CONTROL_TYPE, jsonBytes) ?: return@repeat
            if (!sendQueue.trySend(record).isSuccess) Log.w(TAG, "Dropping control event: secure queue full")
        }
    }

    private fun handleClientDisconnect(reason: String) {
        Log.d(TAG, "Secure client disconnected: $reason")
        disconnectClient()
        if (serverSocket != null && !serverSocket!!.isClosed) {
            _connectionState.value = ConnectionState.LISTENING
            _errorMessage.value = reason
        }
    }

    private fun disconnectClient() {
        pendingPairingJob?.cancel()
        pendingPairingJob = null
        readJob?.cancel(); writeJob?.cancel(); heartbeatJob?.cancel()
        readJob = null; writeJob = null; heartbeatJob = null
        _connectedSenderName.value = null
        try { inputStream?.close() } catch (_: Exception) {}
        try { outputStream?.close() } catch (_: Exception) {}
        try { clientSocket?.close() } catch (_: Exception) {}
        try { pendingPairingSocket?.close() } catch (_: Exception) {}
        inputStream = null; outputStream = null; clientSocket = null
        pendingSession = null; pendingPairingSocket = null; secureSession = null
        while (sendQueue.tryReceive().isSuccess) { }
    }

    fun disconnect() {
        disconnectClient()
        _connectionState.value = ConnectionState.LISTENING
        _errorMessage.value = null
    }

    fun stopListening() {
        disconnectClient()
        acceptJob?.cancel(); acceptJob = null
        try { serverSocket?.close() } catch (_: Exception) {}
        serverSocket = null; listeningPort = 0
        _connectionState.value = ConnectionState.IDLE
        _errorMessage.value = null
    }

    fun destroy() {
        stopListening()
        scope.cancel()
    }
}

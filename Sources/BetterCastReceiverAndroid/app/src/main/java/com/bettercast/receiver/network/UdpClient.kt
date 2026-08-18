package com.bettercast.receiver.network

import android.util.Log
import com.bettercast.receiver.input.InputEvent
import kotlinx.coroutines.*
import kotlinx.coroutines.channels.Channel
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.nio.ByteBuffer
import java.nio.ByteOrder

class UdpClient(private val port: Int) {

    companion object {
        private const val TAG = "UdpClient"
        private const val MAX_PACKET_SIZE = 65535
        private const val MAX_UDP_CHUNKS = 2048
        private const val MAX_FRAMES_IN_FLIGHT = 16
        private const val MAX_BYTES_IN_FLIGHT = 32 * 1024 * 1024
        private const val MAX_FRAME_SIZE = 8 * 1024 * 1024
        private const val STALE_FRAME_TIMEOUT_MS = 500L
        private const val CLEANUP_INTERVAL = 100
        private const val HEARTBEAT_INTERVAL_MS = 5_000L
        const val DEFAULT_PORT = 51820
    }

    private data class FrameBuffer(
        val totalChunks: Int,
        val chunks: MutableMap<Int, ByteArray>,
        val timestamp: Long,
        var totalBytes: Int = 0
    )

    private var socket: DatagramSocket? = null
    private var receiveJob: Job? = null
    private var heartbeatJob: Job? = null
    private var sendJob: Job? = null
    private var cleanupCounter = 0

    private val frameBuffers = mutableMapOf<Long, FrameBuffer>()
    private var bytesInFlight = 0
    private var lastDecodedFrameId: Long = 0

    // Track sender address for sending heartbeats/input back
    @Volatile private var senderAddress: InetAddress? = null
    @Volatile private var senderPort: Int = 0
    @Volatile var isSenderConnected: Boolean = false
        private set

    private val sendQueue = Channel<ByteArray>(Channel.BUFFERED)

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    var onFrameReassembled: ((ByteArray) -> Unit)? = null
    var onGapDetected: (() -> Unit)? = null
    var onSenderConnected: (() -> Unit)? = null

    fun start() {
        if (receiveJob != null) return

        receiveJob = scope.launch {
            try {
                val sock = DatagramSocket(null) // create unbound
                sock.reuseAddress = true
                sock.bind(InetSocketAddress(port)) // bind after setting reuseAddress
                socket = sock
                Log.d(TAG, "UDP listening on port $port")

                val buffer = ByteArray(MAX_PACKET_SIZE)
                val packet = DatagramPacket(buffer, buffer.size)

                while (isActive) {
                    packet.length = buffer.size
                    sock.receive(packet)

                    // Track sender address for bidirectional communication
                    val newAddr = packet.address
                    val newPort = packet.port
                    if (senderAddress != null && (senderAddress != newAddr || senderPort != newPort)) {
                        // Do not allow an unauthenticated datagram to retarget the session.
                        continue
                    }
                    if (senderAddress == null) {
                        senderAddress = newAddr
                        senderPort = newPort
                        if (!isSenderConnected) {
                            isSenderConnected = true
                            Log.d(TAG, "Sender connected from $newAddr:$newPort")
                            startHeartbeat()
                            startSendLoop()
                            onSenderConnected?.invoke()
                        }
                    }

                    val data = packet.data.copyOfRange(packet.offset, packet.offset + packet.length)
                    handlePacket(data)
                }
            } catch (e: Exception) {
                if (isActive) {
                    Log.e(TAG, "UDP receive error", e)
                }
            }
        }
    }

    private fun handlePacket(data: ByteArray) {
        if (data.size <= 8 || data.size > MAX_PACKET_SIZE) return

        val header = ByteBuffer.wrap(data, 0, 8).order(ByteOrder.BIG_ENDIAN)
        val frameId = header.int.toLong() and 0xFFFFFFFFL
        val chunkId = header.short.toInt() and 0xFFFF
        val totalChunks = header.short.toInt() and 0xFFFF
        if (totalChunks <= 0 || totalChunks > MAX_UDP_CHUNKS || chunkId >= totalChunks) return

        val payload = data.copyOfRange(8, data.size)
        var completeFrame: ByteArray? = null
        var gapDetected = false

        synchronized(frameBuffers) {
            if (lastDecodedFrameId == 0L) {
                lastDecodedFrameId = frameId - 1
            }

            val current = frameBuffers[frameId]
            if (current == null) {
                if (frameBuffers.size >= MAX_FRAMES_IN_FLIGHT || bytesInFlight + payload.size > MAX_BYTES_IN_FLIGHT) {
                    return@synchronized
                }
                frameBuffers[frameId] = FrameBuffer(totalChunks, mutableMapOf(), System.currentTimeMillis())
            }

            val frame = frameBuffers[frameId] ?: return@synchronized
            if (frame.totalChunks != totalChunks) {
                bytesInFlight -= frame.totalBytes
                frameBuffers.remove(frameId)
                return@synchronized
            }

            if (!frame.chunks.containsKey(chunkId)) {
                if (bytesInFlight + payload.size > MAX_BYTES_IN_FLIGHT) return@synchronized
                frame.chunks[chunkId] = payload
                frame.totalBytes += payload.size
                bytesInFlight += payload.size
            }

            val complete = frame.chunks.size == frame.totalChunks &&
                (0 until frame.totalChunks).all { frame.chunks.containsKey(it) }
            if (complete) {
                if (frame.totalBytes > MAX_FRAME_SIZE) {
                    bytesInFlight -= frame.totalBytes
                    frameBuffers.remove(frameId)
                    return@synchronized
                }
                val sortedChunks = (0 until frame.totalChunks).mapNotNull { frame.chunks[it] }
                completeFrame = ByteArray(frame.totalBytes)
                var offset = 0
                for (chunk in sortedChunks) {
                    System.arraycopy(chunk, 0, completeFrame!!, offset, chunk.size)
                    offset += chunk.size
                }
                gapDetected = frameId - lastDecodedFrameId > 1 && frameId - lastDecodedFrameId < 1000
                lastDecodedFrameId = frameId
                bytesInFlight -= frame.totalBytes
                frameBuffers.remove(frameId)
            }

            cleanupCounter++
            if (cleanupCounter >= CLEANUP_INTERVAL) {
                cleanupStaleFrames()
                cleanupCounter = 0
            }
        }

        if (gapDetected) onGapDetected?.invoke()
        completeFrame?.let { onFrameReassembled?.invoke(it) }
    }

    private fun cleanupStaleFrames() {
        val now = System.currentTimeMillis()
        val staleIds = frameBuffers.entries
            .filter { now - it.value.timestamp > STALE_FRAME_TIMEOUT_MS }
            .map { it.key }
        for (id in staleIds) {
            val frame = frameBuffers.remove(id)
            bytesInFlight -= frame?.totalBytes ?: 0
        }
    }

    private fun startHeartbeat() {
        heartbeatJob?.cancel()
        heartbeatJob = scope.launch {
            while (isActive && isSenderConnected) {
                delay(HEARTBEAT_INTERVAL_MS)
                sendInputEvent(InputEvent.heartbeat())
            }
        }
    }

    private fun startSendLoop() {
        sendJob?.cancel()
        sendJob = scope.launch {
            val sock = socket ?: return@launch
            try {
                for (data in sendQueue) {
                    if (!isActive) break
                    val addr = senderAddress ?: continue
                    val p = senderPort
                    try {
                        val packet = DatagramPacket(data, data.size, addr, p)
                        sock.send(packet)
                    } catch (e: Exception) {
                        Log.e(TAG, "UDP send failed to $addr:$p: ${e.message}")
                    }
                }
            } catch (e: Exception) {
                if (isActive) {
                    Log.e(TAG, "UDP send loop error", e)
                }
            }
        }
    }

    fun sendInputEvent(event: InputEvent) {
        val repeatCount = if (InputEvent.isCritical(event.type)) 3 else 1
        val json = Json.encodeToString(event)
        val jsonBytes = json.toByteArray(Charsets.UTF_8)

        // Length-prefixed JSON (same format as TCP)
        val packet = ByteBuffer.allocate(4 + jsonBytes.size)
        packet.putInt(jsonBytes.size)
        packet.put(jsonBytes)
        val packetBytes = packet.array()

        repeat(repeatCount) {
            sendQueue.trySend(packetBytes)
        }
    }

    fun stop() {
        isSenderConnected = false
        senderAddress = null
        senderPort = 0

        heartbeatJob?.cancel()
        sendJob?.cancel()
        receiveJob?.cancel()
        heartbeatJob = null
        sendJob = null
        receiveJob = null

        try { socket?.close() } catch (_: Exception) {}
        socket = null

        synchronized(frameBuffers) {
            frameBuffers.clear()
            bytesInFlight = 0
        }
    }

    fun destroy() {
        stop()
        scope.cancel()
    }
}

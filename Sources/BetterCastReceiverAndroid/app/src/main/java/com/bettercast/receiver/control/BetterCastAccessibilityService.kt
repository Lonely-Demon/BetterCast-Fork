package com.bettercast.receiver.control

import android.accessibilityservice.AccessibilityService
import android.accessibilityservice.GestureDescription
import android.graphics.Path
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.accessibility.AccessibilityEvent
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.long
import kotlinx.serialization.json.double

/**
 * Explicitly user-enabled Android control endpoint for BetterCast Phone Control mode.
 *
 * This service is intentionally fail-closed: it accepts only the small command set
 * required by the session protocol, validates all coordinates and durations, and
 * never executes shell commands or arbitrary key codes.
 */
class BetterCastAccessibilityService : AccessibilityService() {

    companion object {
        private const val TAG = "BetterCastA11y"
        private const val MAX_CONTROL_BYTES = 16 * 1024
        private const val MAX_GESTURE_MS = 5_000L
        private const val MAX_COORDINATE = 1.0
        private val json = Json { ignoreUnknownKeys = false; isLenient = false }

        @Volatile
        private var activeService: BetterCastAccessibilityService? = null

        @Volatile
        private var sessionArmed = false

        fun isEnabled(): Boolean = activeService != null

        fun setSessionArmed(armed: Boolean) {
            sessionArmed = armed
        }

        fun handleControlPayload(payload: ByteArray): Boolean {
            if (!sessionArmed || payload.isEmpty() || payload.size > MAX_CONTROL_BYTES) return false
            val service = activeService ?: return false
            return service.dispatchPayload(payload)
        }
    }

    private val mainHandler = Handler(Looper.getMainLooper())

    override fun onServiceConnected() {
        super.onServiceConnected()
        activeService = this
        Log.i(TAG, "Accessibility control service enabled")
    }

    override fun onAccessibilityEvent(event: AccessibilityEvent?) {
        // BetterCast does not inspect window contents. The service is used only
        // for explicitly authorized gestures and allow-listed global actions.
    }

    override fun onInterrupt() {
        Log.i(TAG, "Accessibility control interrupted")
    }

    override fun onDestroy() {
        if (activeService === this) {
            activeService = null
            sessionArmed = false
        }
        super.onDestroy()
    }

    private fun dispatchPayload(payload: ByteArray): Boolean {
        val root = try {
            json.parseToJsonElement(payload.toString(Charsets.UTF_8)).jsonObject
        } catch (error: Exception) {
            Log.w(TAG, "Rejected malformed control payload", error)
            return false
        }

        val command = root["command"]?.jsonPrimitive?.content ?: return false
        return when (command) {
            "tap" -> {
                val x = normalizedCoordinate(root, "x") ?: return false
                val y = normalizedCoordinate(root, "y") ?: return false
                dispatchTap(x, y)
            }
            "swipe" -> {
                val x1 = normalizedCoordinate(root, "x1") ?: return false
                val y1 = normalizedCoordinate(root, "y1") ?: return false
                val x2 = normalizedCoordinate(root, "x2") ?: return false
                val y2 = normalizedCoordinate(root, "y2") ?: return false
                val duration = boundedDuration(root["durationMs"]?.jsonPrimitive?.long ?: 250L)
                dispatchSwipe(x1, y1, x2, y2, duration)
            }
            "global" -> {
                val action = root["action"]?.jsonPrimitive?.content ?: return false
                dispatchGlobalAction(action)
            }
            else -> false
        }
    }

    private fun normalizedCoordinate(root: kotlinx.serialization.json.JsonObject, key: String): Float? {
        val value = try {
            root[key]?.jsonPrimitive?.double
        } catch (_: Exception) {
            null
        } ?: return null
        if (!value.isFinite() || value < 0.0 || value > MAX_COORDINATE) return null
        return value.toFloat()
    }

    private fun boundedDuration(value: Long): Long = value.coerceIn(1L, MAX_GESTURE_MS)

    private fun screenPoint(x: Float, y: Float): Pair<Float, Float> {
        val metrics = resources.displayMetrics
        return Pair(x * metrics.widthPixels, y * metrics.heightPixels)
    }

    private fun dispatchTap(x: Float, y: Float): Boolean {
        val (px, py) = screenPoint(x, y)
        val path = Path().apply { moveTo(px, py) }
        val gesture = GestureDescription.Builder()
            .addStroke(GestureDescription.StrokeDescription(path, 0L, 1L))
            .build()
        return dispatchGesture(gesture, null, mainHandler)
    }

    private fun dispatchSwipe(x1: Float, y1: Float, x2: Float, y2: Float, duration: Long): Boolean {
        val (sx, sy) = screenPoint(x1, y1)
        val (ex, ey) = screenPoint(x2, y2)
        val path = Path().apply {
            moveTo(sx, sy)
            lineTo(ex, ey)
        }
        val gesture = GestureDescription.Builder()
            .addStroke(GestureDescription.StrokeDescription(path, 0L, duration))
            .build()
        return dispatchGesture(gesture, null, mainHandler)
    }

    private fun dispatchGlobalAction(action: String): Boolean {
        val globalAction = when (action) {
            "back" -> GLOBAL_ACTION_BACK
            "home" -> GLOBAL_ACTION_HOME
            "recents" -> GLOBAL_ACTION_RECENTS
            "notifications" -> GLOBAL_ACTION_NOTIFICATIONS
            else -> return false
        }
        return performGlobalAction(globalAction)
    }
}

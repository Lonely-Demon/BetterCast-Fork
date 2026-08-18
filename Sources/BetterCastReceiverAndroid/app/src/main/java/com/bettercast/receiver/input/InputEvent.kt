package com.bettercast.receiver.input

import kotlinx.serialization.Serializable
import java.util.concurrent.atomic.AtomicLong
import kotlin.math.abs

@Serializable
data class InputEvent(
    val type: Int,
    val x: Double,
    val y: Double,
    val keyCode: Int,
    val deltaX: Double,
    val deltaY: Double,
    val eventId: Long
) {
    companion object {
        const val TYPE_MOUSE_MOVE = 0
        const val TYPE_LEFT_MOUSE_DOWN = 1
        const val TYPE_LEFT_MOUSE_UP = 2
        const val TYPE_RIGHT_MOUSE_DOWN = 3
        const val TYPE_RIGHT_MOUSE_UP = 4
        const val TYPE_KEY_DOWN = 5
        const val TYPE_KEY_UP = 6
        const val TYPE_SCROLL_WHEEL = 7
        const val TYPE_COMMAND = 99

        const val COMMAND_HEARTBEAT = 888
        const val COMMAND_REQUEST_KEYFRAME = 999

        private val idCounter = AtomicLong(0)

        fun nextId(): Long = idCounter.incrementAndGet()

        fun heartbeat(): InputEvent = InputEvent(
            type = TYPE_COMMAND,
            x = 0.0,
            y = 0.0,
            keyCode = COMMAND_HEARTBEAT,
            deltaX = 0.0,
            deltaY = 0.0,
            eventId = nextId()
        )

        fun requestKeyframe(): InputEvent = InputEvent(
            type = TYPE_COMMAND,
            x = 0.0,
            y = 0.0,
            keyCode = COMMAND_REQUEST_KEYFRAME,
            deltaX = 0.0,
            deltaY = 0.0,
            eventId = nextId()
        )

        fun mouseMove(x: Double, y: Double): InputEvent = InputEvent(
            type = TYPE_MOUSE_MOVE,
            x = x, y = y,
            keyCode = 0, deltaX = 0.0, deltaY = 0.0,
            eventId = nextId()
        )

        fun leftMouseDown(x: Double, y: Double): InputEvent = InputEvent(
            type = TYPE_LEFT_MOUSE_DOWN,
            x = x, y = y,
            keyCode = 0, deltaX = 0.0, deltaY = 0.0,
            eventId = nextId()
        )

        fun leftMouseUp(x: Double, y: Double): InputEvent = InputEvent(
            type = TYPE_LEFT_MOUSE_UP,
            x = x, y = y,
            keyCode = 0, deltaX = 0.0, deltaY = 0.0,
            eventId = nextId()
        )

        fun rightMouseDown(x: Double, y: Double): InputEvent = InputEvent(
            type = TYPE_RIGHT_MOUSE_DOWN,
            x = x, y = y,
            keyCode = 0, deltaX = 0.0, deltaY = 0.0,
            eventId = nextId()
        )

        fun rightMouseUp(x: Double, y: Double): InputEvent = InputEvent(
            type = TYPE_RIGHT_MOUSE_UP,
            x = x, y = y,
            keyCode = 0, deltaX = 0.0, deltaY = 0.0,
            eventId = nextId()
        )

        fun scroll(x: Double, y: Double, deltaX: Double, deltaY: Double): InputEvent = InputEvent(
            type = TYPE_SCROLL_WHEEL,
            x = x, y = y,
            keyCode = 0, deltaX = deltaX, deltaY = deltaY,
            eventId = nextId()
        )

        fun isCritical(type: Int): Boolean = type in listOf(
            TYPE_LEFT_MOUSE_DOWN, TYPE_LEFT_MOUSE_UP,
            TYPE_RIGHT_MOUSE_DOWN, TYPE_RIGHT_MOUSE_UP,
            TYPE_KEY_DOWN, TYPE_KEY_UP, TYPE_COMMAND
        )
        fun isValid(event: InputEvent): Boolean {
            val knownType = event.type in setOf(
                TYPE_MOUSE_MOVE, TYPE_LEFT_MOUSE_DOWN, TYPE_LEFT_MOUSE_UP,
                TYPE_RIGHT_MOUSE_DOWN, TYPE_RIGHT_MOUSE_UP, TYPE_KEY_DOWN,
                TYPE_KEY_UP, TYPE_SCROLL_WHEEL, TYPE_COMMAND
            )
            if (!knownType || event.eventId <= 0L || event.keyCode !in 0..0xFFFF) return false
            if (event.type == TYPE_COMMAND && event.keyCode !in setOf(COMMAND_HEARTBEAT, COMMAND_REQUEST_KEYFRAME, 777)) return false
            if (!event.x.isFinite() || !event.y.isFinite() ||
                !event.deltaX.isFinite() || !event.deltaY.isFinite()) return false
            if (event.type != TYPE_COMMAND && (event.x !in 0.0..1.0 || event.y !in 0.0..1.0)) return false
            if (abs(event.deltaX) > 10_000.0 || abs(event.deltaY) > 10_000.0) return false
            return true
        }
    }
}

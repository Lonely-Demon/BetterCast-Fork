package com.bettercast.receiver

import com.bettercast.receiver.input.InputEvent
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class InputEventValidationTest {
    @Test
    fun validMouseMoveIsAccepted() {
        assertTrue(InputEvent.isValid(InputEvent.mouseMove(0.5, 0.5)))
    }

    @Test
    fun outOfRangeCoordinatesAreRejected() {
        assertFalse(InputEvent.isValid(InputEvent.mouseMove(-0.1, 0.5)))
        assertFalse(InputEvent.isValid(InputEvent.mouseMove(0.5, 1.1)))
    }

    @Test
    fun nonFiniteCoordinatesAreRejected() {
        val event = InputEvent(
            type = InputEvent.TYPE_MOUSE_MOVE,
            x = Double.NaN,
            y = 0.5,
            keyCode = 0,
            deltaX = 0.0,
            deltaY = 0.0,
            eventId = 1L
        )
        assertFalse(InputEvent.isValid(event))
    }

    @Test
    fun unknownCommandsAreRejected() {
        val event = InputEvent(
            type = InputEvent.TYPE_COMMAND,
            x = 0.0,
            y = 0.0,
            keyCode = 1234,
            deltaX = 0.0,
            deltaY = 0.0,
            eventId = 1L
        )
        assertFalse(InputEvent.isValid(event))
    }

    @Test
    fun excessiveScrollIsRejected() {
        val event = InputEvent.scroll(0.5, 0.5, 0.0, 10_001.0)
        assertFalse(InputEvent.isValid(event))
    }
}

package com.bettercast.receiver.ui

import android.content.Intent
import android.provider.Settings
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.viewinterop.AndroidView
import com.bettercast.receiver.input.TouchHandler
import com.bettercast.receiver.viewmodel.ReceiverState
import com.bettercast.receiver.viewmodel.ReceiverViewModel
import kotlinx.coroutines.delay

@Composable
fun ReceiverScreen(viewModel: ReceiverViewModel) {
    val context = LocalContext.current
    val state by viewModel.state.collectAsState()
    val statusMessage by viewModel.statusMessage.collectAsState()
    val deviceIp by viewModel.deviceIp.collectAsState()
    val phoneControlEnabled by viewModel.phoneControlEnabled.collectAsState()
    val phoneControlStatus by viewModel.phoneControlStatus.collectAsState()
    val pairingCode by viewModel.pairingCode.collectAsState()
    val pairingFingerprint by viewModel.pairingFingerprint.collectAsState()

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.Black)
    ) {
        when (state) {
            ReceiverState.WAITING -> {
                WaitingView(
                    statusMessage = statusMessage,
                    deviceIp = deviceIp,
                    port = viewModel.tcpServer.listeningPort,
                    phoneControlEnabled = phoneControlEnabled,
                    phoneControlStatus = phoneControlStatus,
                    pairingCode = pairingCode,
                    pairingFingerprint = pairingFingerprint,
                    onApprovePairing = { viewModel.approvePairing() },
                    onOpenAccessibilitySettings = {
                        context.startActivity(Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS))
                    },
                    onTogglePhoneControl = { viewModel.setPhoneControlEnabled(!phoneControlEnabled) }
                )
            }

            ReceiverState.RECONNECTING -> {
                ReconnectingView(statusMessage = statusMessage)
            }

            ReceiverState.CONNECTED -> {
                ConnectedView(viewModel = viewModel, statusMessage = statusMessage)
            }

            ReceiverState.ERROR -> {
                ErrorView(
                    statusMessage = statusMessage,
                    onRetry = { viewModel.retry() }
                )
            }
        }
    }
}

@Composable
private fun WaitingView(
    statusMessage: String,
    deviceIp: String?,
    port: Int,
    phoneControlEnabled: Boolean,
    phoneControlStatus: String,
    pairingCode: String?,
    pairingFingerprint: String?,
    onApprovePairing: () -> Boolean,
    onOpenAccessibilitySettings: () -> Unit,
    onTogglePhoneControl: () -> Unit
) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center
    ) {
        Text(
            text = "BetterCast",
            fontSize = 32.sp,
            fontWeight = FontWeight.Bold,
            color = Color.White
        )

        Spacer(modifier = Modifier.height(8.dp))

        Text(
            text = "Android Receiver",
            fontSize = 16.sp,
            color = Color.Gray
        )

        Spacer(modifier = Modifier.height(32.dp))

        CircularProgressIndicator(
            modifier = Modifier.size(48.dp),
            color = MaterialTheme.colorScheme.primary,
            strokeWidth = 3.dp
        )

        Spacer(modifier = Modifier.height(16.dp))

        Text(
            text = statusMessage,
            fontSize = 14.sp,
            color = Color.Gray,
            textAlign = TextAlign.Center
        )

        if (deviceIp != null && port > 0) {
            Spacer(modifier = Modifier.height(24.dp))

            Text(
                text = "Connect manually:",
                fontSize = 12.sp,
                color = Color.Gray
            )

            Spacer(modifier = Modifier.height(4.dp))

            Text(
                text = "$deviceIp:$port",
                fontSize = 20.sp,
                fontWeight = FontWeight.Bold,
                color = Color(0xFF64B5F6),
                textAlign = TextAlign.Center
            )
        }

        if (pairingCode != null) {
            Spacer(modifier = Modifier.height(16.dp))
            Text(
                text = "Secure pairing",
                fontSize = 13.sp,
                fontWeight = FontWeight.SemiBold,
                color = Color(0xFFFFD54F)
            )
            Spacer(modifier = Modifier.height(4.dp))
            Text(
                text = "Compare this code with Windows before approving.",
                fontSize = 11.sp,
                color = Color(0xFFAAAAAA),
                textAlign = TextAlign.Center
            )
            Text(
                text = pairingCode,
                fontSize = 20.sp,
                fontWeight = FontWeight.Bold,
                color = Color(0xFFFFD54F)
            )
            if (!pairingFingerprint.isNullOrBlank()) {
                Text(
                    text = "Peer fingerprint: ${pairingFingerprint.take(16)}…",
                    fontSize = 9.sp,
                    color = Color(0xFF777777),
                    textAlign = TextAlign.Center
                )
            }
            Spacer(modifier = Modifier.height(4.dp))
            Button(
                onClick = { onApprovePairing() },
                colors = ButtonDefaults.buttonColors(containerColor = Color(0xFF2E7D32)),
                shape = RoundedCornerShape(8.dp)
            ) {
                Text("Approve pairing", fontSize = 12.sp)
            }
        }

        Spacer(modifier = Modifier.height(20.dp))

        Text(
            text = "Windows Phone Control",
            fontSize = 13.sp,
            fontWeight = FontWeight.SemiBold,
            color = Color(0xFFAAAAAA)
        )

        Spacer(modifier = Modifier.height(6.dp))

        Text(
            text = "Use the Windows mouse on this phone over Wi-Fi while keeping the normal Android UI visible.",
            fontSize = 11.sp,
            color = Color(0xFF777777),
            textAlign = TextAlign.Center,
            modifier = Modifier.padding(horizontal = 24.dp)
        )

        Spacer(modifier = Modifier.height(8.dp))

        Row(
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Button(
                onClick = onOpenAccessibilitySettings,
                colors = ButtonDefaults.buttonColors(containerColor = Color(0xFF333333)),
                shape = RoundedCornerShape(8.dp)
            ) {
                Text("Accessibility", fontSize = 12.sp)
            }

            Button(
                onClick = onTogglePhoneControl,
                colors = ButtonDefaults.buttonColors(
                    containerColor = if (phoneControlEnabled) Color(0xFFB71C1C) else Color(0xFF1565C0)
                ),
                shape = RoundedCornerShape(8.dp)
            ) {
                Text(if (phoneControlEnabled) "Disarm Control" else "Arm Control", fontSize = 12.sp)
            }
        }

        Text(
            text = phoneControlStatus,
            fontSize = 11.sp,
            color = if (phoneControlEnabled) Color(0xFF81C784) else Color(0xFFAAAAAA),
            textAlign = TextAlign.Center,
            modifier = Modifier.padding(horizontal = 24.dp, vertical = 4.dp)
        )

        Spacer(modifier = Modifier.height(20.dp))

        Text(
            text = "Windows Wi-Fi setup",
            fontSize = 13.sp,
            fontWeight = FontWeight.SemiBold,
            color = Color(0xFFAAAAAA)
        )

        Spacer(modifier = Modifier.height(8.dp))

        Text(
            text = "1. Keep this phone and the Windows laptop on the same Wi-Fi network.\n" +
                    "2. Start BetterCast on Windows and choose Send Screen or Phone Control.\n" +
                    "3. Select this receiver, or enter the address shown above.\n" +
                    "4. For Phone Control, enable BetterCast Accessibility once in Android Settings.",
            fontSize = 11.sp,
            color = Color(0xFF777777),
            textAlign = TextAlign.Start,
            lineHeight = 16.sp,
            modifier = Modifier.padding(horizontal = 32.dp)
        )
    }
}

@Composable
private fun ReconnectingView(statusMessage: String) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center
    ) {
        CircularProgressIndicator(
            modifier = Modifier.size(48.dp),
            color = Color(0xFF64B5F6),
            strokeWidth = 3.dp
        )

        Spacer(modifier = Modifier.height(16.dp))

        Text(
            text = statusMessage,
            fontSize = 16.sp,
            fontWeight = FontWeight.Medium,
            color = Color.White,
            textAlign = TextAlign.Center
        )

        Spacer(modifier = Modifier.height(8.dp))

        Text(
            text = "Please wait while the sender reconnects...",
            fontSize = 13.sp,
            color = Color.Gray,
            textAlign = TextAlign.Center
        )
    }
}

@Composable
private fun ConnectedView(viewModel: ReceiverViewModel, statusMessage: String) {
    var showOverlay by remember { mutableStateOf(true) }

    // Auto-hide overlay after 3 seconds
    LaunchedEffect(Unit) {
        delay(3000)
        showOverlay = false
    }

    Box(modifier = Modifier.fillMaxSize()) {
        // SurfaceView for video rendering
        AndroidView(
            factory = { context ->
                SurfaceView(context).apply {
                    holder.addCallback(object : SurfaceHolder.Callback {
                        override fun surfaceCreated(holder: SurfaceHolder) {
                            viewModel.videoDecoder.setSurface(holder.surface)
                        }

                        override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                        }

                        override fun surfaceDestroyed(holder: SurfaceHolder) {
                            viewModel.videoDecoder.setSurface(null)
                        }
                    })

                    // Attach touch handler
                    val touchHandler = TouchHandler(this) { event ->
                        viewModel.sendInputEvent(event)
                    }

                    // Set video rect to full surface initially
                    post {
                        touchHandler.updateVideoRect(0f, 0f, width.toFloat(), height.toFloat())
                    }
                }
            },
            modifier = Modifier.fillMaxSize()
        )

        // Status overlay
        AnimatedVisibility(
            visible = showOverlay,
            enter = fadeIn(),
            exit = fadeOut(),
            modifier = Modifier.align(Alignment.TopCenter)
        ) {
            Box(
                modifier = Modifier
                    .padding(top = 16.dp)
                    .background(
                        color = Color(0x99000000),
                        shape = RoundedCornerShape(8.dp)
                    )
                    .padding(horizontal = 16.dp, vertical = 8.dp)
            ) {
                Text(
                    text = statusMessage,
                    fontSize = 14.sp,
                    color = Color.White
                )
            }
        }

        // Disconnect button (top-right, subtle)
        AnimatedVisibility(
            visible = showOverlay,
            enter = fadeIn(),
            exit = fadeOut(),
            modifier = Modifier
                .align(Alignment.TopEnd)
                .padding(16.dp)
        ) {
            Button(
                onClick = { viewModel.disconnect() },
                colors = ButtonDefaults.buttonColors(
                    containerColor = Color(0x66FF0000)
                ),
                shape = RoundedCornerShape(8.dp)
            ) {
                Text("Disconnect", fontSize = 12.sp)
            }
        }
    }
}

@Composable
private fun ErrorView(
    statusMessage: String,
    onRetry: () -> Unit
) {
    Column(
        modifier = Modifier.fillMaxSize(),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center
    ) {
        Text(
            text = "Error",
            fontSize = 24.sp,
            fontWeight = FontWeight.Bold,
            color = Color(0xFFFF5252)
        )

        Spacer(modifier = Modifier.height(8.dp))

        Text(
            text = statusMessage,
            fontSize = 14.sp,
            color = Color.Gray,
            textAlign = TextAlign.Center,
            modifier = Modifier.padding(horizontal = 32.dp)
        )

        Spacer(modifier = Modifier.height(24.dp))

        Button(
            onClick = onRetry,
            colors = ButtonDefaults.buttonColors(
                containerColor = MaterialTheme.colorScheme.primary
            ),
            shape = RoundedCornerShape(12.dp)
        ) {
            Text("Retry", fontSize = 16.sp)
        }
    }
}

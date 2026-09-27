package org.openapex.androidauto

import android.Manifest
import android.annotation.SuppressLint
import android.app.ActivityOptions
import android.app.Notification
import android.app.PendingIntent
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.service.notification.StatusBarNotification
import android.telecom.TelecomManager
import android.util.Log

/**
 * Incoming calls to the terminal, and the terminal's Accept back to the phone.
 *
 * Source is the dialer's own notification, read by [NavNotificationRelayService]: it already
 * carries the caller as the dialer resolved it (contact name, else the number), so this needs no
 * contacts, call-log or phone-state permission. Any app posting a proper call notification --
 * Google/Samsung dialer, WhatsApp, Signal -- is picked up the same way.
 *
 * Accept fires the notification's own answer PendingIntent, so it answers exactly the call on
 * screen in whichever app owns it. [TelecomManager.acceptRingingCall] is only a fallback for a
 * carrier call whose notification exposes no answer action.
 *
 * There is no reject path: the rider lets it ring out and the notification's removal clears it.
 */
object CallRelay {
    private const val TAG = "CallRelay"

    // Mirrors CALL_RING_REFRESH_MS (firmware/main/call_state.h): the terminal drops a ring that is
    // not refreshed, so a lost CLEAR can never strand the call page.
    private const val RING_REFRESH_MS = 5000L

    // Accept-by-label fallback for pre-CallStyle dialers (API < 31), which post plain actions.
    private val ANSWER_LABELS = listOf("answer", "accept", "pick up")

    private data class Ringing(val key: String, val callId: Int, val caller: String?, val answer: PendingIntent?)

    private val handler = Handler(Looper.getMainLooper())
    private var ringing: Ringing? = null
    private var nextCallId = 1
    private var appContext: Context? = null

    private val refresh = object : Runnable {
        override fun run() {
            val r = ringing ?: return
            send(CALL_EVENT_RING, r)
            handler.postDelayed(this, RING_REFRESH_MS)
        }
    }

    /** Every posted notification. Cheap for anything that is not a call. */
    fun onPosted(context: Context, sbn: StatusBarNotification) {
        val n = sbn.notification
        if (n.category != Notification.CATEGORY_CALL) return
        val current = ringing
        if (!isIncoming(n)) {
            // The same notification reposted as an ongoing call (answered, here or on the phone).
            if (current != null && current.key == sbn.key) clear()
            return
        }
        appContext = context.applicationContext
        val caller = n.extras.getCharSequence(Notification.EXTRA_TITLE)?.toString()?.trim()
        val callId = if (current != null && current.key == sbn.key) current.callId else allocateId()
        val next = Ringing(sbn.key, callId, caller, answerIntent(n))
        val changed = next != current
        ringing = next
        if (changed) {
            Log.i(TAG, "ringing id=$callId caller=$caller answerAction=${next.answer != null}")
            handler.removeCallbacks(refresh)
            refresh.run()
        }
    }

    fun onRemoved(sbn: StatusBarNotification) {
        if (ringing?.key == sbn.key) clear()
    }

    /** Terminal tapped Accept (CALL_ACTION notify). Main thread. */
    fun onAccept(callId: Int) {
        val r = ringing
        if (r == null || r.callId != callId) {
            Log.w(TAG, "accept id=$callId ignored: ringing=${r?.callId}")
            return
        }
        val answered = r.answer?.let { sendAnswer(it) } == true || acceptViaTelecom()
        Log.i(TAG, "accept id=$callId answered=$answered")
    }

    private fun clear() {
        val r = ringing ?: return
        ringing = null
        handler.removeCallbacks(refresh)
        Log.i(TAG, "cleared id=${r.callId}")
        send(CALL_EVENT_CLEAR, r)
    }

    private fun send(type: Int, r: Ringing) =
        RelayStateHolder.sendCallEvent(packCallEvent(type, r.callId, r.caller))

    private fun allocateId(): Int {
        val id = nextCallId
        nextCallId = if (nextCallId >= 0xFFFF) 1 else nextCallId + 1
        return id
    }

    private fun isIncoming(n: Notification): Boolean {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            // CallStyle states it outright; 0 = not a CallStyle notification, fall through.
            when (n.extras.getInt(Notification.EXTRA_CALL_TYPE, 0)) {
                Notification.CallStyle.CALL_TYPE_INCOMING -> return true
                Notification.CallStyle.CALL_TYPE_ONGOING,
                Notification.CallStyle.CALL_TYPE_SCREENING -> return false
            }
        }
        return answerIntent(n) != null
    }

    private fun answerIntent(n: Notification): PendingIntent? {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            n.extras.getParcelable(Notification.EXTRA_ANSWER_INTENT, PendingIntent::class.java)?.let { return it }
        } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            @Suppress("DEPRECATION")
            (n.extras.getParcelable<PendingIntent>(Notification.EXTRA_ANSWER_INTENT))?.let { return it }
        }
        val action = n.actions?.firstOrNull { a ->
            (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && a.semanticAction == Notification.Action.SEMANTIC_ACTION_CALL) ||
                ANSWER_LABELS.any { a.title?.toString()?.contains(it, ignoreCase = true) == true }
        }
        return action?.actionIntent
    }

    /**
     * The answer intent usually starts the dialer's in-call activity. From API 34 a PendingIntent
     * sent by a background app may only start an activity if the SENDER opts in; without this the
     * send "succeeds" and nothing happens.
     */
    private fun sendAnswer(pi: PendingIntent): Boolean = try {
        val options = when {
            Build.VERSION.SDK_INT >= 36 -> ActivityOptions.makeBasic()
                .setPendingIntentBackgroundActivityStartMode(ActivityOptions.MODE_BACKGROUND_ACTIVITY_START_ALLOW_ALWAYS)
                .toBundle()
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE -> ActivityOptions.makeBasic()
                .setPendingIntentBackgroundActivityStartMode(
                    @Suppress("DEPRECATION") ActivityOptions.MODE_BACKGROUND_ACTIVITY_START_ALLOWED,
                )
                .toBundle()
            else -> null
        }
        pi.send(appContext, 0, null, null, null, null, options)
        true
    } catch (e: PendingIntent.CanceledException) {
        Log.w(TAG, "answer intent cancelled", e)
        false
    }

    /** Carrier-call fallback. Deprecated (API 29) but still functional; needs ANSWER_PHONE_CALLS. */
    @SuppressLint("MissingPermission")
    private fun acceptViaTelecom(): Boolean {
        val ctx = appContext ?: return false
        if (ctx.checkSelfPermission(Manifest.permission.ANSWER_PHONE_CALLS) != PackageManager.PERMISSION_GRANTED) {
            Log.w(TAG, "no answer action and ANSWER_PHONE_CALLS not granted")
            return false
        }
        val telecom = ctx.getSystemService(TelecomManager::class.java) ?: return false
        @Suppress("DEPRECATION")
        telecom.acceptRingingCall()
        return true
    }
}

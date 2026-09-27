package org.openapex.androidauto

import android.Manifest
import android.annotation.SuppressLint
import android.app.ActivityOptions
import android.app.Notification
import android.app.PendingIntent
import android.app.Person
import android.content.ContentUris
import android.content.Context
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.provider.ContactsContract
import android.service.notification.StatusBarNotification
import android.telecom.TelecomManager
import android.util.Log

/**
 * Incoming calls to the terminal, and the terminal's Accept back to the phone.
 *
 * Source is the dialer's own notification, read by [NavNotificationRelayService]: it already
 * carries the caller as the dialer resolved it (contact name, else the number), so this needs no
 * call-log or phone-state permission. Any app posting a proper call notification -- Google/Samsung
 * dialer, WhatsApp, Signal -- is picked up the same way.
 *
 * The number comes from the notification's caller Person (a `tel:` uri) or its text. A saved
 * contact's Person usually carries a contacts uri instead, and that only resolves to a number with
 * the optional READ_CONTACTS grant; without it the terminal shows the name alone.
 *
 * A call is tracked from its first incoming post: RING while it rings, ACTIVE once the same
 * notification is reposted as ongoing (answered here or on the phone), CLEAR when it is removed.
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

    private data class Ringing(
        val key: String,
        val callId: Int,
        val caller: String?,
        val number: String?,
        val answer: PendingIntent?,
        val active: Boolean = false,
    )

    private val PHONE_NUMBER = Regex("""^\+?[\d\s().-]{5,}$""")
    private val NUMBER_IN_TEXT = Regex("""\+?\d[\d\s().-]{4,}\d""")

    private val handler = Handler(Looper.getMainLooper())
    private var ringing: Ringing? = null
    private var nextCallId = 1
    private var appContext: Context? = null

    private val refresh = object : Runnable {
        override fun run() {
            val r = ringing ?: return
            send(if (r.active) CALL_EVENT_ACTIVE else CALL_EVENT_RING, r)
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
            // An ongoing call never seen ringing (outgoing) is not tracked.
            if (current != null && current.key == sbn.key && !current.active) {
                ringing = current.copy(active = true, answer = null)
                Log.i(TAG, "active id=${current.callId}")
                handler.removeCallbacks(refresh)
                refresh.run()
            }
            return
        }
        appContext = context.applicationContext
        val title = n.extras.getCharSequence(Notification.EXTRA_TITLE)?.toString()?.trim()
        val titleIsNumber = title != null && PHONE_NUMBER.matches(title)
        val caller = if (titleIsNumber) null else title
        val number = callerNumber(context, n) ?: title.takeIf { titleIsNumber }
        val callId = if (current != null && current.key == sbn.key) current.callId else allocateId()
        val next = Ringing(sbn.key, callId, caller, number, answerIntent(n))
        val changed = next != current
        ringing = next
        if (changed) {
            Log.i(TAG, "ringing id=$callId caller=$caller number=${number != null} answerAction=${next.answer != null}")
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
        if (r == null || r.callId != callId || r.active) {
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
        RelayStateHolder.sendCallEvent(packCallEvent(type, r.callId, r.caller, r.number))

    private fun allocateId(): Int {
        val id = nextCallId
        nextCallId = if (nextCallId >= 0xFFFF) 1 else nextCallId + 1
        return id
    }

    /** The caller's number from the notification's Person or text, or null. */
    private fun callerNumber(context: Context, n: Notification): String? {
        val person: Person? = when {
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU ->
                n.extras.getParcelable(Notification.EXTRA_CALL_PERSON, Person::class.java)
                    ?: n.extras.getParcelableArrayList(Notification.EXTRA_PEOPLE_LIST, Person::class.java)?.firstOrNull()
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.S -> @Suppress("DEPRECATION") (
                n.extras.getParcelable<Person>(Notification.EXTRA_CALL_PERSON)
                    ?: n.extras.getParcelableArrayList<Person>(Notification.EXTRA_PEOPLE_LIST)?.firstOrNull()
                )
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.P -> @Suppress("DEPRECATION")
                n.extras.getParcelableArrayList<Person>(Notification.EXTRA_PEOPLE_LIST)?.firstOrNull()
            else -> null
        }
        val uri = person?.uri?.let(Uri::parse)
        if (uri?.scheme == "tel") uri.schemeSpecificPart?.takeIf { it.isNotBlank() }?.let { return it }
        if (uri?.scheme == "content") contactNumber(context, uri)?.let { return it }
        // Some dialers (Samsung) put the number in the text under the contact name.
        return listOf(Notification.EXTRA_TEXT, Notification.EXTRA_SUB_TEXT)
            .firstNotNullOfOrNull { k -> n.extras.getCharSequence(k)?.let { NUMBER_IN_TEXT.find(it)?.value } }
    }

    /** First number of a contact uri, primary first. Only with the optional READ_CONTACTS grant. */
    private fun contactNumber(context: Context, uri: Uri): String? {
        if (context.checkSelfPermission(Manifest.permission.READ_CONTACTS) != PackageManager.PERMISSION_GRANTED) return null
        return try {
            val resolver = context.contentResolver
            val contact = ContactsContract.Contacts.lookupContact(resolver, uri) ?: return null
            resolver.query(
                ContactsContract.CommonDataKinds.Phone.CONTENT_URI,
                arrayOf(ContactsContract.CommonDataKinds.Phone.NUMBER),
                "${ContactsContract.CommonDataKinds.Phone.CONTACT_ID}=?",
                arrayOf(ContentUris.parseId(contact).toString()),
                "${ContactsContract.CommonDataKinds.Phone.IS_SUPER_PRIMARY} DESC",
            )?.use { c -> if (c.moveToFirst()) c.getString(0) else null }
        } catch (e: RuntimeException) {
            // Not a lookup uri this provider understands; the name alone is fine.
            Log.w(TAG, "contact number lookup failed for $uri", e)
            null
        }
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

package com.einx.send

import android.content.Context

/** The reader this phone sends to unless told otherwise. */
data class DefaultReader(val address: String, val name: String)

/**
 * What the app remembers between sends.
 *
 * Only the chosen reader and the "always ask" preference — no book history, no file paths. The
 * point of remembering the reader is that sending a second book should not mean picking the same
 * device out of a list again.
 */
class SendPreferences(context: Context) {

  private val prefs = context.getSharedPreferences("einx_send", Context.MODE_PRIVATE)

  var defaultReader: DefaultReader?
    get() {
      val address = prefs.getString(KEY_ADDRESS, null) ?: return null
      val name = prefs.getString(KEY_NAME, null) ?: address
      return DefaultReader(address, name)
    }
    set(value) {
      prefs.edit().apply {
        if (value == null) {
          remove(KEY_ADDRESS)
          remove(KEY_NAME)
        } else {
          putString(KEY_ADDRESS, value.address)
          putString(KEY_NAME, value.name)
        }
      }.apply()
    }

  /** When set, a remembered reader is still offered but never pre-selected. */
  var alwaysAsk: Boolean
    get() = prefs.getBoolean(KEY_ALWAYS_ASK, false)
    set(value) = prefs.edit().putBoolean(KEY_ALWAYS_ASK, value).apply()

  private companion object {
    const val KEY_ADDRESS = "default_reader_address"
    const val KEY_NAME = "default_reader_name"
    const val KEY_ALWAYS_ASK = "always_ask"
  }
}

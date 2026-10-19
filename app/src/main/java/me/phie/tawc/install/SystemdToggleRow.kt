package me.phie.tawc.install

import android.app.Activity
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.CheckBox
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import me.phie.tawc.R
import java.util.concurrent.Executor

/**
 * Settings distro card toggle for [Installation.systemdBoot] (tawcroot
 * installs). Commits like [buildAndoCommitRow]: on [executor], gated
 * to READY/FAILED. Enabling applies to the next spawn; disabling powers
 * off a running session ([TawcrootMethod.stopSystemd]).
 */
internal fun buildSystemdCommitRow(
    activity: Activity,
    store: InstallationStore,
    installation: Installation,
    executor: Executor,
): LinearLayout {
    val container = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }
    var reverting = false
    container.addView(
        CheckBox(activity).apply {
            text = activity.getString(R.string.systemd_toggle_label)
            isChecked = installation.systemdBoot
            setOnCheckedChangeListener { checkbox, checked ->
                if (reverting) return@setOnCheckedChangeListener
                executor.execute {
                    val saved = store.update(installation.id) { current ->
                        if (current.state == Installation.State.READY ||
                            current.state == Installation.State.FAILED
                        ) {
                            current.copy(systemdBoot = checked)
                        } else {
                            null
                        }
                    }
                    if (saved != null) {
                        if (!checked) {
                            TawcrootMethod(activity).stopSystemd(store.rootfsDir(installation.id).absolutePath)
                        }
                    } else {
                        activity.runOnUiThread {
                            Toast.makeText(
                                activity,
                                activity.getString(R.string.systemd_toggle_failed),
                                Toast.LENGTH_SHORT,
                            ).show()
                            reverting = true
                            checkbox.isChecked = !checked
                            reverting = false
                        }
                    }
                }
            }
        },
        LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT),
    )
    container.addView(
        TextView(activity).apply {
            text = activity.getString(R.string.systemd_toggle_description)
            textSize = 12f
            alpha = 0.7f
        },
        LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT),
    )
    return container
}

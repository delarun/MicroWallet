// Установка скачанного APK поверх себя через PackageInstaller. В первый раз Android спросит,
// можно ли MicroWallet ставить приложения; дальше (Android 12+) обновление идёт без вопросов:
// приложение само себя и ставило. Подпись APK должна совпадать с установленной.
package io.microwallet.app.ui

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.os.Build
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue

object AppInstaller {
    private const val ACTION = "io.microwallet.app.INSTALL_RESULT"

    /** Ошибка последней установки (null — нет): показывает экран обновления. */
    var error by mutableStateOf<String?>(null)

    fun install(ctx: Context, apk: ByteArray) {
        error = null
        val installer = ctx.packageManager.packageInstaller
        val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL).apply {
            setAppPackageName(ctx.packageName)
            if (Build.VERSION.SDK_INT >= 31) setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED)
        }
        val id = installer.createSession(params)
        installer.openSession(id).use { s ->
            s.openWrite("microwallet.apk", 0, apk.size.toLong()).use {
                it.write(apk)
                s.fsync(it)
            }
            val intent = Intent(ctx, InstallReceiver::class.java).setAction(ACTION)
            val flags = PendingIntent.FLAG_UPDATE_CURRENT or (if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0)
            s.commit(PendingIntent.getBroadcast(ctx, id, intent, flags).intentSender)
        }
    }
}

/** Итог установки: попросить подтверждение у человека или запомнить ошибку. */
class InstallReceiver : BroadcastReceiver() {
    override fun onReceive(ctx: Context, intent: Intent) {
        when (intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)) {
            PackageInstaller.STATUS_PENDING_USER_ACTION -> {
                val confirm = if (Build.VERSION.SDK_INT >= 33) intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent::class.java)
                else @Suppress("DEPRECATION") intent.getParcelableExtra(Intent.EXTRA_INTENT)
                confirm?.let { ctx.startActivity(it.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) }
            }
            PackageInstaller.STATUS_SUCCESS -> Unit  // процесс сейчас перезапустится в новой версии
            else -> AppInstaller.error = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE) ?: "установка не удалась"
        }
    }
}

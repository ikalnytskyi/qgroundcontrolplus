package org.mavlink.qgroundcontrol;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

public class QGCBootReceiver extends BroadcastReceiver {
    private static final String TAG = QGCBootReceiver.class.getSimpleName();
    private static final String ACTION_QUICKBOOT_POWERON = "android.intent.action.QUICKBOOT_POWERON";

    @Override
    public void onReceive(Context context, Intent intent) {
        if (intent == null || intent.getAction() == null) {
            return;
        }

        final String action = intent.getAction();
        final boolean shouldLaunch =
            Intent.ACTION_BOOT_COMPLETED.equals(action)
                || Intent.ACTION_LOCKED_BOOT_COMPLETED.equals(action)
                || Intent.ACTION_MY_PACKAGE_REPLACED.equals(action)
                || ACTION_QUICKBOOT_POWERON.equals(action);

        if (!shouldLaunch) {
            QGCLogger.i(TAG, "Ignoring broadcast action: " + action);
            return;
        }

        final Intent launchIntent = new Intent(context, QGCActivity.class);
        launchIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        launchIntent.putExtra("org.mavlink.qgroundcontrol.AUTO_LAUNCHED", true);
        launchIntent.putExtra("org.mavlink.qgroundcontrol.AUTO_LAUNCH_ACTION", action);

        try {
            context.startActivity(launchIntent);
            QGCLogger.i(TAG, "Launched QGCActivity after broadcast: " + action);
        } catch (Exception e) {
            QGCLogger.e(TAG, "Failed to launch QGCActivity after broadcast: " + action, e);
        }
    }
}

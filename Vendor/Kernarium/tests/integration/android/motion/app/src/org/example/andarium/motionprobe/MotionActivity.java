// SPDX-License-Identifier: Apache-2.0
package org.example.andarium.motionprobe;

import android.app.Activity;
import android.content.Intent;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.SystemClock;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.TextView;
import org.json.JSONObject;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Locale;

/** A real foreground SensorManager client; never installed in a product image. */
public final class MotionActivity extends Activity implements SensorEventListener {
    private final Handler handler = new Handler();
    private SensorManager manager;
    private TextView text;
    private Samples acceleration, rotation;
    private String mode;
    private String runId;
    private long started, deadline;
    private int seconds;
    private boolean valid, completed;

    private static final class Samples {
        int count;
        long first, last;
        double sum, peak;
        final float[] low = {Float.POSITIVE_INFINITY, Float.POSITIVE_INFINITY, Float.POSITIVE_INFINITY};
        final float[] high = {Float.NEGATIVE_INFINITY, Float.NEGATIVE_INFINITY, Float.NEGATIVE_INFINITY};
        final float[] latest = new float[3];

        boolean add(SensorEvent event) {
            if (event.timestamp <= last || event.timestamp > SystemClock.elapsedRealtimeNanos() + 1_000_000_000L) return false;
            double squared = 0;
            for (int i = 0; i < 3; i++) {
                float value = event.values[i];
                if (!Float.isFinite(value)) return false;
                latest[i] = value;
                low[i] = Math.min(low[i], value);
                high[i] = Math.max(high[i], value);
                squared += (double) value * value;
            }
            double norm = Math.sqrt(squared);
            sum += norm;
            peak = Math.max(peak, norm);
            if (count++ == 0) first = event.timestamp;
            last = event.timestamp;
            return true;
        }

        double span() {
            double span = 0;
            if (count != 0) for (int i = 0; i < 3; i++) span = Math.max(span, high[i] - low[i]);
            return span;
        }

        JSONObject json() throws Exception {
            return new JSONObject().put("count", count).put("first_ns", first).put("last_ns", last)
                .put("mean_norm", count == 0 ? 0 : sum / count).put("peak_norm", peak).put("span", span());
        }
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        text = new TextView(this);
        text.setTextSize(20);
        text.setPadding(28, 28, 28, 28);
        LinearLayout layout = new LinearLayout(this);
        layout.addView(text);
        setContentView(layout);
        manager = getSystemService(SensorManager.class);
        begin(getIntent());
    }

    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        begin(intent);
    }

    private void begin(Intent intent) {
        handler.removeCallbacksAndMessages(null);
        manager.unregisterListener(this);
        acceleration = new Samples();
        rotation = new Samples();
        mode = intent.getStringExtra("mode");
        runId = intent.getStringExtra("run_id");
        if (mode == null) mode = "events";
        seconds = Math.max(1, Math.min(240, intent.getIntExtra("seconds", 5)));
        started = SystemClock.elapsedRealtimeNanos();
        deadline = started + seconds * 1_000_000_000L;
        completed = false;
        Sensor accel = manager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
        Sensor gyro = manager.getDefaultSensor(Sensor.TYPE_GYROSCOPE);
        valid = accel != null && gyro != null && "Host Accelerometer".equals(accel.getName())
            && "Host Gyroscope".equals(gyro.getName());
        if (valid) valid = manager.registerListener(this, accel, 20_000)
            && manager.registerListener(this, gyro, 20_000);
        new File(getFilesDir(), "result.json").delete();
        handler.post(tick);
    }

    private boolean passed() {
        if (!valid) return false;
        if (mode.equals("silence")) return acceleration.count == 0 && rotation.count == 0;
        if (acceleration.count < 75 || rotation.count < 75) return false;
        if (mode.equals("movement")) return acceleration.span() > 0.5 && rotation.peak > 0.05;
        return acceleration.sum / acceleration.count > 7 && acceleration.sum / acceleration.count < 12;
    }

    private final Runnable tick = new Runnable() {
        @Override public void run() {
            boolean done = !valid || SystemClock.elapsedRealtimeNanos() >= deadline
                || (mode.equals("movement") && passed());
            if (!completed && done) {
                completed = true;
                try {
                    JSONObject report = new JSONObject().put("mode", mode).put("run_id", runId)
                        .put("valid", valid).put("passed", passed())
                        .put("accelerometer", acceleration.json()).put("gyroscope", rotation.json());
                    File temporary = new File(getFilesDir(), "result.json.new");
                    try (FileOutputStream output = new FileOutputStream(temporary)) {
                        output.write(report.toString().getBytes(StandardCharsets.UTF_8));
                    }
                    if (!temporary.renameTo(new File(getFilesDir(), "result.json"))) throw new Exception("report rename failed");
                } catch (Exception error) { throw new IllegalStateException(error); }
            }
            text.setText(String.format(Locale.ROOT,
                "Motion Probe\n%s%s\n\nAccelerometer (m/s²)\nx %.3f   y %.3f   z %.3f\nSamples: %d\n\nGyroscope (rad/s)\nx %.3f   y %.3f   z %.3f\nSamples: %d",
                mode, completed ? (passed() ? " — passed" : " — failed") : " — collecting",
                acceleration.latest[0], acceleration.latest[1], acceleration.latest[2], acceleration.count,
                rotation.latest[0], rotation.latest[1], rotation.latest[2], rotation.count));
            handler.postDelayed(this, 200);
        }
    };

    @Override public void onSensorChanged(SensorEvent event) {
        if (event.timestamp < started) return;
        if (event.sensor.getType() == Sensor.TYPE_ACCELEROMETER) valid &= acceleration.add(event);
        else if (event.sensor.getType() == Sensor.TYPE_GYROSCOPE) valid &= rotation.add(event);
    }
    @Override public void onAccuracyChanged(Sensor sensor, int accuracy) {}
    @Override protected void onDestroy() {
        handler.removeCallbacksAndMessages(null);
        manager.unregisterListener(this);
        super.onDestroy();
    }
}

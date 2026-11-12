// SPDX-License-Identifier: Apache-2.0
package org.example.andarium.webglprobe;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.SystemClock;
import android.view.WindowManager;
import android.webkit.RenderProcessGoneDetail;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import org.json.JSONObject;

/** The same public Aquarium workload the desktop WebGL runner uses, in Android WebView. */
public final class WebGLActivity extends Activity {
    private static final String SAMPLE = "(() => {"
        + "const c=document.getElementById('canvas');"
        + "if(!c)return null;const g=c.getContext('webgl2')||c.getContext('webgl');"
        + "if(!g)return {error:'WebGL context unavailable'};"
        + "if(!window.andariumFrames){const f={count:0};window.andariumFrames=f;"
        + "const tick=()=>{f.count++;requestAnimationFrame(tick)};requestAnimationFrame(tick)}"
        + "const e=g.getExtension('WEBGL_debug_renderer_info');"
        + "return {fps:parseFloat(document.getElementById('fps')?.textContent||'0'),"
        + "frames:window.andariumFrames.count,"
        + "width:c.width,height:c.height,lost:g.isContextLost(),"
        + "version:g.getParameter(g.VERSION),"
        + "renderer:g.getParameter(e?e.UNMASKED_RENDERER_WEBGL:g.RENDERER)};})()";
    private final Handler handler = new Handler();
    private WebView web;
    private String runId;
    private long deadline;
    private int stable, samples;
    private long previousFrames = -1;
    private double fpsSum, fpsMinimum = Double.POSITIVE_INFINITY;
    private boolean complete;
    private JSONObject latest = new JSONObject();
    private int width, height;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        runId = getIntent().getStringExtra("run_id");
        width = getResources().getDisplayMetrics().widthPixels;
        height = getResources().getDisplayMetrics().heightPixels;
        deadline = SystemClock.elapsedRealtime() + 180_000;
        web = new WebView(this);
        web.getSettings().setJavaScriptEnabled(true);
        web.getSettings().setDomStorageEnabled(true);
        web.getSettings().setUseWideViewPort(true);
        web.getSettings().setLoadWithOverviewMode(true);
        web.setWebViewClient(new WebViewClient() {
            @Override public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error) {
                if (request.isForMainFrame()) finish(false, "Page load failed: " + error.getDescription());
            }
            @Override public boolean onRenderProcessGone(WebView view, RenderProcessGoneDetail detail) {
                finish(false, "WebView renderer exited; crash=" + detail.didCrash());
                return true;
            }
        });
        setContentView(web);
        write(false, false, "loading");
        web.loadUrl("https://webglsamples.org/aquarium/aquarium.html?numFish=5000"
            + "&canvasWidth=" + width + "&canvasHeight=" + height + "&fitWindow=false");
        handler.postDelayed(tick, 1000);
    }

    private final Runnable tick = new Runnable() {
        @Override public void run() {
            if (complete) return;
            if (SystemClock.elapsedRealtime() >= deadline) { finish(false, "WebGL deadline expired"); return; }
            web.evaluateJavascript(SAMPLE, value -> {
                if (complete) return;
                try {
                    if (!"null".equals(value)) {
                        latest = new JSONObject(value);
                        if (latest.has("error") || latest.optBoolean("lost")) {
                            finish(false, latest.optString("error", "WebGL context lost")); return;
                        }
                        String renderer = latest.optString("renderer").toLowerCase();
                        if (renderer.contains("swiftshader") || renderer.contains("llvmpipe")) {
                            finish(false, "Software renderer: " + renderer); return;
                        }
                        double fps = latest.optDouble("fps", 0);
                        long frames = latest.optLong("frames");
                        if (Double.isFinite(fps) && fps > 0 && frames > previousFrames
                                && latest.optInt("width") == width && latest.optInt("height") == height) {
                            stable++;
                            if (stable > 15) {
                                samples++;
                                fpsSum += fps;
                                fpsMinimum = Math.min(fpsMinimum, fps);
                            }
                            if (samples >= 60) { finish(true, "complete"); return; }
                            write(false, false, stable <= 15 ? "warming" : "measuring");
                        }
                        previousFrames = frames;
                    }
                } catch (Exception error) { finish(false, error.toString()); return; }
                handler.postDelayed(this, 1000);
            });
        }
    };

    private void finish(boolean passed, String message) {
        if (complete) return;
        complete = true;
        write(true, passed, message);
    }

    private void write(boolean done, boolean passed, String message) {
        try {
            JSONObject report = new JSONObject().put("run_id", runId).put("done", done)
                .put("passed", passed).put("message", message).put("fish", 5000)
                .put("samples", samples).put("average_fps", samples == 0 ? 0 : fpsSum / samples)
                .put("minimum_fps", samples == 0 ? 0 : fpsMinimum).put("webgl", latest);
            File temporary = new File(getFilesDir(), "result.json.new");
            try (FileOutputStream output = new FileOutputStream(temporary)) {
                output.write(report.toString().getBytes(StandardCharsets.UTF_8));
            }
            if (!temporary.renameTo(new File(getFilesDir(), "result.json"))) throw new Exception("Report rename failed");
        } catch (Exception error) { throw new IllegalStateException(error); }
    }

    @Override protected void onDestroy() {
        complete = true;
        handler.removeCallbacksAndMessages(null);
        if (web != null) { web.stopLoading(); web.destroy(); web = null; }
        super.onDestroy();
    }
}

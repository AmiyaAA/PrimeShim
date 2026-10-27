package dev.primeshim.android;

import android.graphics.Color;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.webkit.JavascriptInterface;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;

/**
 * A tiny JavaScript host for user scripts.
 *
 * The script runs in a hidden WebView; the JS bridge forwards key and touch
 * calls to the same native entry points used by the Android input views.
 * No network, file or DOM access is enabled.
 */
public final class ScriptRuntime {
    public interface Listener {
        void onScriptLog(String line);
        void onScriptFinished();
    }

    private static final String PAGE =
            "<!doctype html><html><body style='margin:0;background:transparent'></body></html>";
    private static final String BASE_URL = "https://primeshim.local/";

    private static final String PRELUDE =
            "(function(){\n" +
            "  window.prime = {\n" +
            "    key: function(name, down){ __prime.key(String(name), down !== false); },\n" +
            "    press: async function(name){\n" +
            "      __prime.key(String(name), true);\n" +
            "      await new Promise(function(r){ setTimeout(r, 60); });\n" +
            "      __prime.key(String(name), false);\n" +
            "    },\n" +
            "    tapKey: function(name){ return prime.press(name); },\n" +
            "    keyDown: function(name){ __prime.key(String(name), true); },\n" +
            "    keyUp: function(name){ __prime.key(String(name), false); },\n" +
            "    touchDown: function(id, x, y){ __prime.touch(1, id|0, x|0, y|0); },\n" +
            "    touchMove: function(id, x, y){ __prime.touch(2, id|0, x|0, y|0); },\n" +
            "    touchUp:   function(id, x, y){ __prime.touch(3, id|0, x|0, y|0); },\n" +
            "    touchTap:  async function(id, x, y){\n" +
            "      prime.touchDown(id, x, y);\n" +
            "      await new Promise(function(r){ setTimeout(r, 60); });\n" +
            "      prime.touchUp(id, x, y);\n" +
            "    },\n" +
            "    sleep: function(ms){ return new Promise(function(r){ setTimeout(r, Math.max(0, ms|0)); }); },\n" +
            "    log: function(){ __prime.log(Array.prototype.join.call(arguments, ' ')); },\n" +
            "    keys: function(){ return __prime.keys(); }\n" +
            "  };\n" +
            "})();\n";

    private final MainActivity activity;
    private final FrameLayout host;
    private final Listener listener;
    private WebView webView;
    private boolean ready;
    private String pendingScript;

    public ScriptRuntime(MainActivity activity, FrameLayout host, Listener listener) {
        this.activity = activity;
        this.host = host;
        this.listener = listener;
        webView = new WebView(activity);
        WebSettings settings = webView.getSettings();
        settings.setJavaScriptEnabled(true);
        settings.setAllowFileAccess(false);
        settings.setAllowContentAccess(false);
        settings.setDomStorageEnabled(false);
        settings.setBlockNetworkLoads(true);
        webView.setBackgroundColor(Color.TRANSPARENT);
        webView.setAlpha(0f);
        webView.setClickable(false);
        webView.setFocusable(false);
        webView.setWebViewClient(new WebViewClient() {
            @Override public void onPageFinished(WebView view, String url) {
                if (url != null && url.startsWith("about:blank")) return;
                webView.evaluateJavascript(PRELUDE, null);
                ready = true;
                if (pendingScript != null) {
                    String script = pendingScript;
                    pendingScript = null;
                    execute(script);
                }
            }
        });
        webView.addJavascriptInterface(new Bridge(), "__prime");
        host.addView(webView, new FrameLayout.LayoutParams(1, 1));
        load();
    }

    private void load() {
        ready = false;
        webView.loadDataWithBaseURL(BASE_URL, PAGE, "text/html", "UTF-8", null);
    }

    private void execute(final String script) {
        String token = "window.__primeRun = (window.__primeRun || 0) + 1;\n" +
                "(async function(){\n" +
                "  try {\n" +
                script + "\n" +
                "    __prime.done();\n" +
                "  } catch (e) {\n" +
                "    __prime.fail(String(e && e.stack ? e.stack : e));\n" +
                "  }\n" +
                "})();\n";
        webView.evaluateJavascript(token, null);
    }

    /** Run a script; JS timers and async/await are supported. */
    public void run(String script) {
        if (webView == null) return;
        if (!ready) {
            pendingScript = script;
            load();
            return;
        }
        execute(script);
    }

    /** Abort timers and any script currently running. */
    public void stop() {
        pendingScript = null;
        if (webView == null) return;
        webView.evaluateJavascript("window.prime = null;", null);
        webView.loadUrl("about:blank");
        load();
    }

    public void destroy() {
        pendingScript = null;
        if (webView == null) return;
        host.removeView(webView);
        webView.removeJavascriptInterface("__prime");
        webView.destroy();
        webView = null;
    }

    private void postLog(String line) {
        new Handler(Looper.getMainLooper()).post(() -> listener.onScriptLog(line));
    }

    private void postFinished() {
        new Handler(Looper.getMainLooper()).post(listener::onScriptFinished);
    }

    private final class Bridge {
        @JavascriptInterface
        public void key(String name, boolean down) {
            int code = PrimeKeyboardView.codeForName(name);
            if (code < 0) {
                postLog("Unknown key: " + name);
                return;
            }
            PrimeShim.nativeKeyBatch(code, down ? 1 : 0);
        }

        @JavascriptInterface
        public void touch(int action, int id, int x, int y) {
            if (id < 0 || id > 7 || action < 1 || action > 3) {
                postLog("Invalid touch: id=" + id + " action=" + action);
                return;
            }
            x = Math.max(0, Math.min(319, x));
            y = Math.max(0, Math.min(239, y));
            PrimeShim.nativeTouchMulti(new int[]{action}, new int[]{id},
                    new int[]{x}, new int[]{y});
        }

        @JavascriptInterface
        public void log(String line) {
            postLog(line == null ? "" : line);
        }

        @JavascriptInterface
        public String keys() {
            String[] names = PrimeKeyboardView.keyNames();
            StringBuilder out = new StringBuilder("[");
            for (int i = 0; i < names.length; i++) {
                if (i > 0) out.append(',');
                out.append('"').append(names[i].replace("\\", "\\\\")
                        .replace("\"", "\\\"")).append('"');
            }
            out.append(']');
            return out.toString();
        }

        @JavascriptInterface
        public void done() {
            postLog("— script finished —");
            postFinished();
        }

        @JavascriptInterface
        public void fail(String error) {
            postLog("Error: " + error);
            postFinished();
        }
    }
}

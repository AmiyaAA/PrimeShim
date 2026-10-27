package dev.primeshim.android;

import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Handler;
import android.os.Looper;
import android.text.Editable;
import android.text.InputType;
import android.text.Spannable;
import android.text.TextWatcher;
import android.text.style.ForegroundColorSpan;
import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import androidx.appcompat.widget.AppCompatEditText;

import java.util.Arrays;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** A small code editor with a fixed line-number gutter and JS highlighting. */
public final class CodeEditorView extends LinearLayout {
    private static final Pattern KEYWORDS = Pattern.compile(
            "\\b(await|async|break|case|catch|class|const|continue|debugger|default|delete|do|else|export|extends|finally|for|from|function|if|import|in|instanceof|let|new|null|of|return|static|super|switch|this|throw|true|false|try|typeof|undefined|var|void|while|with|yield)\\b");
    private static final Pattern NUMBERS = Pattern.compile("\\b(?:0[xX][0-9a-fA-F]+|\\d+(?:\\.\\d+)?)\\b");
    private static final Pattern PRIME = Pattern.compile("\\bprime(?:\\.[A-Za-z_$][\\w$]*)?");

    private final AppCompatEditText editor;
    private final TextView gutter;
    private final View divider;
    private final ScrollView verticalScroll;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable gutterTask = this::updateGutter;
    private final Runnable highlightTask = this::highlight;
    private boolean dark;
    private boolean highlighting;

    private int colorKeyword;
    private int colorString;
    private int colorComment;
    private int colorNumber;
    private int colorPrime;

    public CodeEditorView(Context context) {
        super(context);
        setOrientation(HORIZONTAL);

        gutter = new TextView(context);
        gutter.setTypeface(Typeface.MONOSPACE);
        gutter.setTextSize(13);
        gutter.setGravity(Gravity.TOP | Gravity.END);
        gutter.setPadding(0, dp(12), dp(7), 0);
        gutter.setLineSpacing(dp(2), 1f);
        gutter.setClickable(false);
        gutter.setFocusable(false);
        addView(gutter, new LayoutParams(dp(42), LayoutParams.MATCH_PARENT));

        divider = new View(context);
        addView(divider, new LayoutParams(dp(1), LayoutParams.MATCH_PARENT));

        FrameLayout host = new FrameLayout(context);
        addView(host, new LayoutParams(0, LayoutParams.MATCH_PARENT, 1f));

        editor = new AppCompatEditText(context);
        editor.setBackground(null);
        editor.setTypeface(Typeface.MONOSPACE);
        editor.setTextSize(13);
        editor.setGravity(Gravity.TOP | Gravity.START);
        editor.setPadding(dp(12), dp(12), dp(12), dp(12));
        editor.setLineSpacing(dp(2), 1f);
        editor.setHorizontallyScrolling(true);
        editor.setSingleLine(false);
        editor.setInputType(InputType.TYPE_CLASS_TEXT
                | InputType.TYPE_TEXT_FLAG_MULTI_LINE
                | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        editor.setImeOptions(android.view.inputmethod.EditorInfo.IME_FLAG_NO_EXTRACT_UI);

        verticalScroll = new ScrollView(context);
        verticalScroll.setFillViewport(true);
        verticalScroll.setClipToPadding(false);
        verticalScroll.setHorizontalScrollBarEnabled(false);
        verticalScroll.addView(editor, new ScrollView.LayoutParams(
                ScrollView.LayoutParams.WRAP_CONTENT,
                ScrollView.LayoutParams.WRAP_CONTENT));

        HorizontalScrollView horizontalScroll = new HorizontalScrollView(context);
        horizontalScroll.setFillViewport(true);
        horizontalScroll.setSmoothScrollingEnabled(true);
        horizontalScroll.setVerticalScrollBarEnabled(false);
        horizontalScroll.addView(verticalScroll, new HorizontalScrollView.LayoutParams(
                HorizontalScrollView.LayoutParams.WRAP_CONTENT,
                HorizontalScrollView.LayoutParams.MATCH_PARENT));
        host.addView(horizontalScroll, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));

        editor.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start,
                                                     int count, int after) { }

            @Override public void onTextChanged(CharSequence s, int start,
                                                 int before, int count) { }

            @Override public void afterTextChanged(Editable s) {
                handler.removeCallbacks(highlightTask);
                handler.postDelayed(highlightTask, 120);
                handler.removeCallbacks(gutterTask);
                handler.post(gutterTask);
            }
        });
        verticalScroll.setOnScrollChangeListener((v, scrollX, scrollY, oldX, oldY) ->
                gutter.scrollTo(0, scrollY));
        editor.setOnScrollChangeListener((v, scrollX, scrollY, oldX, oldY) ->
                gutter.scrollTo(0, verticalScroll.getScrollY() + scrollY));
        setDark(false);
        updateGutter();
    }

    public void setText(String text) {
        editor.setText(text);
        editor.setSelection(0);
        handler.post(gutterTask);
        handler.post(highlightTask);
    }

    public Editable getText() {
        return editor.getText();
    }

    public void setHint(String hint) {
        editor.setHint(hint);
    }

    public void setDark(boolean dark) {
        this.dark = dark;
        if (dark) {
            editor.setTextColor(0xFFD4D4D4);
            editor.setHintTextColor(0xFF8B949E);
            gutter.setTextColor(0xFF8B949E);
            gutter.setBackgroundColor(0xFF161B22);
            divider.setBackgroundColor(0xFF30363D);
            colorKeyword = 0xFFC678DD;
            colorString = 0xFF98C379;
            colorComment = 0xFF7F848E;
            colorNumber = 0xFFD19A66;
            colorPrime = 0xFF61AFEF;
        } else {
            editor.setTextColor(0xFF1F2328);
            editor.setHintTextColor(0xFF6E7781);
            gutter.setTextColor(0xFF6E7781);
            gutter.setBackgroundColor(0xFFF6F8FA);
            divider.setBackgroundColor(0xFFD0D7DE);
            colorKeyword = 0xFF7C3AED;
            colorString = 0xFF2E7D32;
            colorComment = 0xFF6A737D;
            colorNumber = 0xFFB45309;
            colorPrime = 0xFF1565C0;
        }
        highlight();
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        handler.post(gutterTask);
    }

    @Override
    protected void onDetachedFromWindow() {
        handler.removeCallbacks(highlightTask);
        handler.removeCallbacks(gutterTask);
        super.onDetachedFromWindow();
    }

    private void updateGutter() {
        if (editor == null || gutter == null) return;
        int lines = Math.max(1, editor.getLineCount());
        StringBuilder numbers = new StringBuilder();
        for (int i = 1; i <= lines; i++) {
            if (i > 1) numbers.append('\n');
            numbers.append(i);
        }
        gutter.setText(numbers);
        gutter.scrollTo(0, editor.getScrollY());
    }

    private void highlight() {
        if (editor == null || highlighting) return;
        Editable text = editor.getText();
        if (text == null) return;
        highlighting = true;
        try {
            int length = text.length();
            ForegroundColorSpan[] old = text.getSpans(0, length,
                    ForegroundColorSpan.class);
            for (ForegroundColorSpan span : old) text.removeSpan(span);
            if (length == 0) return;

            boolean[] code = new boolean[length];
            Arrays.fill(code, true);
            String source = text.toString();
            int i = 0;
            while (i < length) {
                char c = source.charAt(i);
                if (c == '/' && i + 1 < length && source.charAt(i + 1) == '/') {
                    int end = source.indexOf('\n', i + 2);
                    if (end < 0) end = length;
                    mark(code, i, end, false);
                    text.setSpan(new ForegroundColorSpan(colorComment), i, end,
                            Spannable.SPAN_EXCLUSIVE_EXCLUSIVE);
                    i = end;
                } else if (c == '/' && i + 1 < length && source.charAt(i + 1) == '*') {
                    int end = source.indexOf("*/", i + 2);
                    end = end < 0 ? length : end + 2;
                    mark(code, i, end, false);
                    text.setSpan(new ForegroundColorSpan(colorComment), i, end,
                            Spannable.SPAN_EXCLUSIVE_EXCLUSIVE);
                    i = end;
                } else if (c == '\'' || c == '"' || c == '`') {
                    int end = i + 1;
                    while (end < length) {
                        char d = source.charAt(end);
                        if (d == '\\') { end += 2; continue; }
                        if (d == c) { ++end; break; }
                        ++end;
                    }
                    if (end > length) end = length;
                    mark(code, i, end, false);
                    text.setSpan(new ForegroundColorSpan(colorString), i, end,
                            Spannable.SPAN_EXCLUSIVE_EXCLUSIVE);
                    i = end;
                } else {
                    ++i;
                }
            }

            applyPattern(text, code, PRIME, colorPrime);
            applyPattern(text, code, KEYWORDS, colorKeyword);
            applyPattern(text, code, NUMBERS, colorNumber);
        } finally {
            highlighting = false;
        }
    }

    private void applyPattern(Editable text, boolean[] code, Pattern pattern, int color) {
        Matcher matcher = pattern.matcher(text);
        while (matcher.find()) {
            int start = matcher.start();
            int end = matcher.end();
            if (!allCode(code, start, end)) continue;
            text.setSpan(new ForegroundColorSpan(color), start, end,
                    Spannable.SPAN_EXCLUSIVE_EXCLUSIVE);
        }
    }

    private static boolean allCode(boolean[] code, int start, int end) {
        for (int i = start; i < end; i++) {
            if (!code[i]) return false;
        }
        return true;
    }

    private static void mark(boolean[] code, int start, int end, boolean value) {
        int limit = Math.min(code.length, end);
        for (int i = Math.max(0, start); i < limit; i++) code[i] = value;
    }

    private int dp(float value) {
        return (int) (value * getResources().getDisplayMetrics().density + 0.5f);
    }
}

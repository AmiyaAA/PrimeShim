package dev.primeshim.android;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PathMeasure;
import android.graphics.RectF;
import android.view.View;

import com.google.android.material.color.MaterialColors;

/**
 * Startup animation for the logo: traces the outline, floods the fill, fades out.
 * The path comes from nb.svg (single path, viewBox 1 1 432 432).
 */
final class SplashView extends View {

    private static final String LOGO_D =
            "M312 23Q269 2 217 1A216 216 0 0 0 98 398l89-247h-34L95 312H49l68-189h84"
            + "c23 0 36 17 28 39l-92 256q39 15 80 15A216 216 0 0 0 352 48l-28 74h34c27 1 39 18 31 40"
            + "l-48 131c-3 10-15 18-26 18H207Zm36 128h-34l-49 132h34Z";

    private static final long TRACE_MS = 2600;
    private static final long FILL_MS = 700;
    private static final long HOLD_MS = 500;
    private static final long FADE_MS = 500;

    private final Path logo = new Path();
    private final PathMeasure measure;
    private final Path trace = new Path();
    private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint strokePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF bounds = new RectF();
    private final long startedAt = System.currentTimeMillis();

    private boolean removing;
    private Runnable onFinished;
    private float logoLength;
    private float scale;
    private float offsetX, offsetY;

    void setOnFinished(Runnable finished) {
        onFinished = finished;
    }

    SplashView(Context context) {
        super(context);
        logo.setFillType(Path.FillType.WINDING);
        Path parsed = androidx.core.graphics.PathParser.createPathFromPathData(LOGO_D);
        if (parsed != null) logo.set(parsed);
        measure = new PathMeasure(logo, true);
        logoLength = 0f;
        do {
            logoLength += measure.getLength();
        } while (measure.nextContour());

        logo.computeBounds(bounds, true);
        /* Material You colour roles: the splash is a normal surface with the
         * wallpaper accent on the logo, so it matches the system launch
         * screen and the rest of the app. */
        int accent = MaterialColors.getColor(context,
                com.google.android.material.R.attr.colorPrimary,
                Color.parseColor("#479FCD"));
        int surface = MaterialColors.getColor(context,
                com.google.android.material.R.attr.colorSurface, Color.WHITE);
        fillPaint.setStyle(Paint.Style.FILL);
        fillPaint.setColor(accent);
        strokePaint.setStyle(Paint.Style.STROKE);
        strokePaint.setStrokeWidth(2.2f);
        strokePaint.setStrokeCap(Paint.Cap.ROUND);
        strokePaint.setStrokeJoin(Paint.Join.ROUND);
        strokePaint.setColor(accent);
        setLayerType(LAYER_TYPE_SOFTWARE, null);
        setBackgroundColor(surface);
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        float target = Math.min(w, h) * 0.42f;
        scale = target / Math.max(bounds.width(), bounds.height());
        offsetX = (w - bounds.width() * scale) / 2f - bounds.left * scale;
        offsetY = (h - bounds.height() * scale) / 2f - bounds.top * scale;
    }

    @Override
    protected void onDraw(Canvas canvas) {
        long elapsed = System.currentTimeMillis() - startedAt;

        canvas.save();
        canvas.translate(offsetX, offsetY);
        canvas.scale(scale, scale);

        float traceFraction = Math.min(1f, elapsed / (float) TRACE_MS);
        if (traceFraction < 1f) {
            /* Draw the outline progressively: one contour at a time. */
            trace.reset();
            float budget = logoLength * ease(traceFraction);
            measure.setPath(logo, true);
            do {
                float len = measure.getLength();
                float take = Math.min(len, budget);
                if (take > 0f) measure.getSegment(0f, take, trace, true);
                budget -= take;
            } while (budget > 0f && measure.nextContour());
            canvas.drawPath(trace, strokePaint);
        } else {
            float fillFraction = Math.min(1f,
                    (elapsed - TRACE_MS) / (float) FILL_MS);
            strokePaint.setAlpha((int) (255 * (1f - fillFraction * 0.0f)));
            canvas.drawPath(logo, strokePaint);
            fillPaint.setAlpha((int) (255 * ease(fillFraction)));
            canvas.drawPath(logo, fillPaint);
        }
        canvas.restore();

        long fadeStart = TRACE_MS + FILL_MS + HOLD_MS;
        if (elapsed > fadeStart) {
            float fade = Math.min(1f, (elapsed - fadeStart) / (float) FADE_MS);
            setAlpha(1f - fade);
            if (fade >= 1f) {
                /* Never mutate the view tree from inside onDraw: post it. */
                if (!removing) {
                    removing = true;
                    post(() -> {
                        if (getParent() instanceof android.view.ViewGroup) {
                            ((android.view.ViewGroup) getParent()).removeView(this);
                        }
                        if (onFinished != null) {
                            Runnable finished = onFinished;
                            onFinished = null;
                            finished.run();
                        }
                    });
                }
                return;
            }
        }
        postInvalidateOnAnimation();
    }

    private static float ease(float t) {
        return 1f - (1f - t) * (1f - t);
    }
}

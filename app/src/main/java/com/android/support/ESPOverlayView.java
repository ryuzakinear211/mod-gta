package com.android.support;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.util.AttributeSet;
import android.view.View;

public class ESPOverlayView extends View {

    private Paint linePaint;
    private Paint lineShadowPaint;
    private Paint boxPaint;
    private Paint boxShadowPaint;
    private Paint textPaint;

    private boolean espLineEnabled = false;
    private boolean espBoxEnabled = false;

    public ESPOverlayView(Context context) {
        super(context);
        init();
    }

    public ESPOverlayView(Context context, AttributeSet attrs) {
        super(context, attrs);
        init();
    }

    private void init() {
        // Outline hitam di belakang garis kuning untuk kontras maksimal di latar terang
        lineShadowPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        lineShadowPaint.setColor(Color.argb(180, 0, 0, 0));
        lineShadowPaint.setStrokeWidth(4.5f);
        lineShadowPaint.setStyle(Paint.Style.STROKE);

        // ESP Line: Berwarna Kuning (Yellow) sesuai tutorial
        linePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        linePaint.setColor(Color.YELLOW);
        linePaint.setStrokeWidth(2.5f);
        linePaint.setStyle(Paint.Style.STROKE);

        // Outline hitam untuk kotak 2D ESP Box
        boxShadowPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        boxShadowPaint.setColor(Color.argb(180, 0, 0, 0));
        boxShadowPaint.setStrokeWidth(4.5f);
        boxShadowPaint.setStyle(Paint.Style.STROKE);

        // ESP Box: Berwarna Kuning (Yellow)
        boxPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        boxPaint.setColor(Color.YELLOW);
        boxPaint.setStrokeWidth(2.5f);
        boxPaint.setStyle(Paint.Style.STROKE);

        // Text Paint untuk indikator jarak (Distance)
        textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        textPaint.setColor(Color.YELLOW);
        textPaint.setTextSize(24f);
        textPaint.setFakeBoldText(true);
        textPaint.setTextAlign(Paint.Align.CENTER);
        textPaint.setShadowLayer(4f, 1f, 1f, Color.BLACK);
    }

    public void setEspLine(boolean enabled) {
        this.espLineEnabled = enabled;
        updateState();
    }

    public void setEspBox(boolean enabled) {
        this.espBoxEnabled = enabled;
        updateState();
    }

    public boolean isEspLineEnabled() {
        return espLineEnabled;
    }

    public boolean isEspBoxEnabled() {
        return espBoxEnabled;
    }

    private void updateState() {
        post(new Runnable() {
            @Override
            public void run() {
                if (espLineEnabled || espBoxEnabled) {
                    setVisibility(View.VISIBLE);
                    requestLayout();
                    invalidate();
                    postInvalidate();
                } else {
                    setVisibility(View.GONE);
                }
            }
        });
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);

        if (!espLineEnabled && !espBoxEnabled) {
            return;
        }

        // Pastikan render loop 60fps selalu terjadwal lebih dulu
        postInvalidateOnAnimation();

        int width = getWidth();
        int height = getHeight();
        if (width <= 0 || height <= 0) return;

        try {
            float[] data = Menu.getEspDrawData(width, height);
            if (data != null && data.length >= 7) {
                int count = data.length / 7;
                float startX = width / 2.0f;
                float startY = 0.0f; // Garis dari atas tengah layar (screenWidth / 2, 0)

                for (int i = 0; i < count; i++) {
                    int offset = i * 7;
                    float headX     = data[offset];
                    float headY     = data[offset + 1];
                    float boxLeft   = data[offset + 2];
                    float boxTop    = data[offset + 3];
                    float boxRight  = data[offset + 4];
                    float boxBottom = data[offset + 5];
                    float dist      = data[offset + 6];

                    // 1. ESP Line (Kuning)
                    if (espLineEnabled) {
                        canvas.drawLine(startX, startY, headX, boxTop, lineShadowPaint);
                        canvas.drawLine(startX, startY, headX, boxTop, linePaint);
                    }

                    // 2. ESP Box
                    if (espBoxEnabled) {
                        canvas.drawRect(boxLeft, boxTop, boxRight, boxBottom, boxShadowPaint);
                        canvas.drawRect(boxLeft, boxTop, boxRight, boxBottom, boxPaint);
                        if (dist > 0.0f) {
                            String distStr = String.format("%.0fm", dist);
                            canvas.drawText(distStr, headX, boxTop - 8.0f, textPaint);
                        }
                    }
                }
            }
        } catch (Throwable ignored) {
        }
    }
}

package com.android.support;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.util.TypedValue;
import android.view.View;

public class ESPOverlayView extends View {

    private Paint mPaintBoxEnemy;
    private Paint mPaintBoxBot;
    private Paint mPaintBoxAlly;
    private Paint mPaintLineEnemy;
    private Paint mPaintLineBot;
    private Paint mPaintLineAlly;
    private Paint mPaintHealthBg;
    private Paint mPaintHealthGreen;
    private Paint mPaintHealthYellow;
    private Paint mPaintHealthRed;
    private Paint mPaintText;
    private Paint mPaintTextShadow;

    private boolean mIsRunning = false;

    public ESPOverlayView(Context context) {
        super(context);
        init();
    }

    private void init() {
        // Box Paints
        mPaintBoxEnemy = new Paint(Paint.ANTI_ALIAS_FLAG);
        mPaintBoxEnemy.setStyle(Paint.Style.STROKE);
        mPaintBoxEnemy.setStrokeWidth(dp(1.8f));
        mPaintBoxEnemy.setColor(Color.parseColor("#FFFF3333")); // Vibrant Red

        mPaintBoxBot = new Paint(Paint.ANTI_ALIAS_FLAG);
        mPaintBoxBot.setStyle(Paint.Style.STROKE);
        mPaintBoxBot.setStrokeWidth(dp(1.8f));
        mPaintBoxBot.setColor(Color.parseColor("#FFFFAA00")); // Vibrant Orange

        mPaintBoxAlly = new Paint(Paint.ANTI_ALIAS_FLAG);
        mPaintBoxAlly.setStyle(Paint.Style.STROKE);
        mPaintBoxAlly.setStrokeWidth(dp(1.8f));
        mPaintBoxAlly.setColor(Color.parseColor("#FF00FF7F")); // Spring Green

        // Tracer Line Paints
        mPaintLineEnemy = new Paint(Paint.ANTI_ALIAS_FLAG);
        mPaintLineEnemy.setStyle(Paint.Style.STROKE);
        mPaintLineEnemy.setStrokeWidth(dp(1.2f));
        mPaintLineEnemy.setColor(Color.parseColor("#CCFF3333"));

        mPaintLineBot = new Paint(Paint.ANTI_ALIAS_FLAG);
        mPaintLineBot.setStyle(Paint.Style.STROKE);
        mPaintLineBot.setStrokeWidth(dp(1.2f));
        mPaintLineBot.setColor(Color.parseColor("#CCFFAA00"));

        mPaintLineAlly = new Paint(Paint.ANTI_ALIAS_FLAG);
        mPaintLineAlly.setStyle(Paint.Style.STROKE);
        mPaintLineAlly.setStrokeWidth(dp(1.2f));
        mPaintLineAlly.setColor(Color.parseColor("#CC00FF7F"));

        // Health Bar Paints
        mPaintHealthBg = new Paint(Paint.ANTI_ALIAS_FLAG);
        mPaintHealthBg.setStyle(Paint.Style.FILL);
        mPaintHealthBg.setColor(Color.parseColor("#CC1A1A1A"));

        mPaintHealthGreen = new Paint(Paint.ANTI_ALIAS_FLAG);
        mPaintHealthGreen.setStyle(Paint.Style.FILL);
        mPaintHealthGreen.setColor(Color.parseColor("#FF00E676"));

        mPaintHealthYellow = new Paint(Paint.ANTI_ALIAS_FLAG);
        mPaintHealthYellow.setStyle(Paint.Style.FILL);
        mPaintHealthYellow.setColor(Color.parseColor("#FFFFD600"));

        mPaintHealthRed = new Paint(Paint.ANTI_ALIAS_FLAG);
        mPaintHealthRed.setStyle(Paint.Style.FILL);
        mPaintHealthRed.setColor(Color.parseColor("#FFFF1744"));

        // Text Paints
        mPaintText = new Paint(Paint.ANTI_ALIAS_FLAG);
        mPaintText.setTextSize(sp(10.0f));
        mPaintText.setColor(Color.WHITE);
        mPaintText.setTextAlign(Paint.Align.CENTER);
        mPaintText.setFakeBoldText(true);

        mPaintTextShadow = new Paint(Paint.ANTI_ALIAS_FLAG);
        mPaintTextShadow.setTextSize(sp(10.0f));
        mPaintTextShadow.setColor(Color.BLACK);
        mPaintTextShadow.setStyle(Paint.Style.STROKE);
        mPaintTextShadow.setStrokeWidth(dp(2.0f));
        mPaintTextShadow.setTextAlign(Paint.Align.CENTER);
        mPaintTextShadow.setFakeBoldText(true);
    }

    public void startLoop() {
        mIsRunning = true;
        setVisibility(VISIBLE);
        postInvalidate();
    }

    public void stopLoop() {
        mIsRunning = false;
        setVisibility(GONE);
        postInvalidate();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);

        if (!mIsRunning || Menu.instance == null || !Menu.instance.IsGameLibLoaded()) {
            return;
        }

        if (!Menu.instance.isEspMasterActive()) {
            return;
        }

        int width = getWidth();
        int height = getHeight();
        if (width <= 0 || height <= 0) {
            postInvalidateDelayed(16);
            return;
        }

        // Fetch flattened entity buffer from native JNI
        float[] entityData = Menu.instance.getNativeEspData(width, height);

        if (entityData != null && entityData.length >= 9) {
            int entityCount = entityData.length / 9;

            boolean drawBox = Menu.instance.isEspBoxActive();
            boolean drawLine = Menu.instance.isEspLineActive();
            boolean drawDistance = Menu.instance.isEspDistanceActive();
            boolean drawHealth = Menu.instance.isEspHealthActive();
            boolean drawName = Menu.instance.isEspNameActive();

            float screenMidX = width / 2.0f;
            float tracerOriginY = 0.0f; // Tracer lines start from top center of screen

            for (int i = 0; i < entityCount; i++) {
                int base = i * 9;
                float rootX = entityData[base + 0];
                float rootY = entityData[base + 1];
                float headX = entityData[base + 2];
                float headY = entityData[base + 3];
                float dist = entityData[base + 4];
                float hp = entityData[base + 5];
                float maxHp = entityData[base + 6];
                boolean isBot = entityData[base + 7] > 0.5f;
                boolean isEnemy = entityData[base + 8] > 0.5f;

                // Box dimensions
                float boxHeight = Math.abs(rootY - headY);
                if (boxHeight < dp(14)) boxHeight = dp(14);
                float boxWidth = boxHeight * 0.52f;

                float boxTop = Math.min(headY, rootY) - (boxHeight * 0.12f);
                float boxBottom = Math.max(headY, rootY) + (boxHeight * 0.06f);
                float boxLeft = rootX - (boxWidth / 2.0f);
                float boxRight = rootX + (boxWidth / 2.0f);

                // Palette selection
                Paint boxPaint = isEnemy ? (isBot ? mPaintBoxBot : mPaintBoxEnemy) : mPaintBoxAlly;
                Paint linePaint = isEnemy ? (isBot ? mPaintLineBot : mPaintLineEnemy) : mPaintLineAlly;

                // 1. Tracer Line
                if (drawLine) {
                    canvas.drawLine(screenMidX, tracerOriginY, headX, boxTop, linePaint);
                }

                // 2. 2D Bounding Box (Corner brackets style for tactical look)
                if (drawBox) {
                    drawCornerBox(canvas, boxLeft, boxTop, boxRight, boxBottom, boxPaint);
                }

                // 3. Health Bar (Vertical Bar on Left)
                if (drawHealth) {
                    float barWidth = dp(3.0f);
                    float barSpacing = dp(3.5f);
                    float barLeft = boxLeft - barSpacing - barWidth;
                    float barRight = boxLeft - barSpacing;

                    // Background Bar
                    canvas.drawRect(barLeft, boxTop, barRight, boxBottom, mPaintHealthBg);

                    // Filled Bar
                    float hpPercent = (maxHp > 0) ? Math.max(0.0f, Math.min(1.0f, hp / maxHp)) : 1.0f;
                    float filledHeight = (boxBottom - boxTop) * hpPercent;
                    float filledTop = boxBottom - filledHeight;

                    Paint hpPaint = (hpPercent > 0.5f) ? mPaintHealthGreen :
                            (hpPercent > 0.2f) ? mPaintHealthYellow : mPaintHealthRed;

                    canvas.drawRect(barLeft, filledTop, barRight, boxBottom, hpPaint);
                }

                // 4. Name & Entity Info (Player / Bot label above box)
                if (drawName) {
                    String label = (isBot ? "[BOT]" : "[PLAYER]") + (isEnemy ? " [E]" : " [A]");
                    float labelY = boxTop - dp(4.0f);
                    drawTextWithOutline(canvas, label, rootX, labelY, boxPaint.getColor());
                }

                // 5. Distance (in meters below box)
                if (drawDistance) {
                    String distStr = String.format("%.0fm", dist);
                    float distY = boxBottom + dp(11.0f);
                    drawTextWithOutline(canvas, distStr, rootX, distY, Color.parseColor("#00E5FF"));
                }
            }
        }

        // Loop next frame (~60 FPS)
        if (mIsRunning) {
            postInvalidateDelayed(16);
        }
    }

    private void drawCornerBox(Canvas canvas, float left, float top, float right, float bottom, Paint paint) {
        float width = right - left;
        float height = bottom - top;
        float cornerLen = Math.min(width, height) * 0.25f;

        // Top-Left Corner
        canvas.drawLine(left, top, left + cornerLen, top, paint);
        canvas.drawLine(left, top, left, top + cornerLen, paint);

        // Top-Right Corner
        canvas.drawLine(right, top, right - cornerLen, top, paint);
        canvas.drawLine(right, top, right, top + cornerLen, paint);

        // Bottom-Left Corner
        canvas.drawLine(left, bottom, left + cornerLen, bottom, paint);
        canvas.drawLine(left, bottom, left, bottom - cornerLen, paint);

        // Bottom-Right Corner
        canvas.drawLine(right, bottom, right - cornerLen, bottom, paint);
        canvas.drawLine(right, bottom, right, bottom - cornerLen, paint);
    }

    private void drawTextWithOutline(Canvas canvas, String text, float x, float y, int color) {
        canvas.drawText(text, x, y, mPaintTextShadow);
        mPaintText.setColor(color);
        canvas.drawText(text, x, y, mPaintText);
    }

    private float dp(float val) {
        return TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, val, getResources().getDisplayMetrics());
    }

    private float sp(float val) {
        return TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, val, getResources().getDisplayMetrics());
    }
}

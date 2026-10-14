package com.android.support;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.view.View;

public class ESPView extends View {

    // Native JNI Interface implemented in libModMenu.so
    public static native int getESPData(float[] outArray, int screenW, int screenH);
    public static native boolean isEspEnabled();
    public static native boolean isEspBox();
    public static native boolean isEspLine();
    public static native boolean isEspHealth();
    public static native boolean isEspDistance();

    // Reusable entity buffer: up to 64 entities * 8 floats = 512 floats (Zero GC allocation per frame)
    private final float[] mBuffer = new float[512];

    // Paints
    private final Paint mPaintLine = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mPaintBox = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mPaintBoxCorner = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mPaintBoxFill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mPaintHealthBg = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mPaintHealthFill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mPaintText = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mPaintTextStroke = new Paint(Paint.ANTI_ALIAS_FLAG);

    // Color definitions
    private static final int COLOR_ENEMY_PLAYER = Color.parseColor("#FF2252"); // Bright Crimson Red
    private static final int COLOR_ENEMY_BOT    = Color.parseColor("#FFAA00"); // Vibrant Amber
    private static final int COLOR_ALLY_PLAYER  = Color.parseColor("#00E5FF"); // Neon Cyan
    private static final int COLOR_ALLY_BOT     = Color.parseColor("#00FF7F"); // Spring Green

    public ESPView(Context context) {
        super(context);
        initPaints();
    }

    private void initPaints() {
        mPaintLine.setStrokeWidth(2.5f);
        mPaintLine.setStyle(Paint.Style.STROKE);

        mPaintBox.setStrokeWidth(1.8f);
        mPaintBox.setStyle(Paint.Style.STROKE);

        mPaintBoxCorner.setStrokeWidth(3.0f);
        mPaintBoxCorner.setStyle(Paint.Style.STROKE);

        mPaintBoxFill.setStyle(Paint.Style.FILL);

        mPaintHealthBg.setStyle(Paint.Style.FILL);
        mPaintHealthBg.setColor(Color.parseColor("#A0000000")); // Semi-transparent black

        mPaintHealthFill.setStyle(Paint.Style.FILL);

        mPaintText.setTextSize(26f);
        mPaintText.setTypeface(Typeface.create(Typeface.MONOSPACE, Typeface.BOLD));
        mPaintText.setTextAlign(Paint.Align.CENTER);
        mPaintText.setColor(Color.WHITE);

        mPaintTextStroke.setTextSize(26f);
        mPaintTextStroke.setTypeface(Typeface.create(Typeface.MONOSPACE, Typeface.BOLD));
        mPaintTextStroke.setTextAlign(Paint.Align.CENTER);
        mPaintTextStroke.setStyle(Paint.Style.STROKE);
        mPaintTextStroke.setStrokeWidth(3.0f);
        mPaintTextStroke.setColor(Color.BLACK);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);

        if (!isEspEnabled()) {
            postInvalidateDelayed(100);
            return;
        }

        int width = getWidth();
        int height = getHeight();
        if (width <= 0 || height <= 0) {
            postInvalidateDelayed(16);
            return;
        }

        boolean drawBox = isEspBox();
        boolean drawLine = isEspLine();
        boolean drawHealth = isEspHealth();
        boolean drawDistance = isEspDistance();

        int count = getESPData(mBuffer, width, height);

        for (int i = 0; i < count; i++) {
            int offset = i * 8;
            int type = (int) mBuffer[offset];
            float headX = mBuffer[offset + 1];
            float headY = mBuffer[offset + 2];
            float rootX = mBuffer[offset + 3];
            float rootY = mBuffer[offset + 4];
            float distance = mBuffer[offset + 5];
            float health = mBuffer[offset + 6];
            int isObstructed = (int) mBuffer[offset + 7];

            int mainColor;
            String tag;
            switch (type) {
                case 1: // Enemy Player
                    mainColor = COLOR_ENEMY_PLAYER;
                    tag = "PLAYER";
                    break;
                case 2: // Enemy Bot
                    mainColor = COLOR_ENEMY_BOT;
                    tag = "BOT";
                    break;
                case 3: // Ally Player
                    mainColor = COLOR_ALLY_PLAYER;
                    tag = "TIM [P]";
                    break;
                case 4: // Ally Bot
                    mainColor = COLOR_ALLY_BOT;
                    tag = "TIM [B]";
                    break;
                default:
                    mainColor = Color.WHITE;
                    tag = "TARGET";
                    break;
            }

            // Calculate Box dimensions
            float boxHeight = Math.max(25f, Math.abs(rootY - headY));
            float boxWidth = boxHeight * 0.55f;
            float topY = Math.min(headY, rootY) - (boxHeight * 0.12f);
            float bottomY = topY + boxHeight * 1.15f;
            float centerX = (headX + rootX) * 0.5f;
            float leftX = centerX - (boxWidth * 0.5f);
            float rightX = centerX + (boxWidth * 0.5f);

            // 1. Draw ESP Line (Snapline from top center)
            if (drawLine) {
                mPaintLine.setColor(mainColor);
                if (isObstructed == 1) {
                    mPaintLine.setAlpha(170);
                } else {
                    mPaintLine.setAlpha(255);
                }
                canvas.drawLine(width * 0.5f, 0, centerX, topY, mPaintLine);
            }

            // 2. Draw ESP Box (2D Tactical Box with Corners)
            if (drawBox) {
                // Subtle fill
                mPaintBoxFill.setColor(mainColor);
                mPaintBoxFill.setAlpha(30);
                canvas.drawRect(leftX, topY, rightX, bottomY, mPaintBoxFill);

                // Thin outline
                mPaintBox.setColor(mainColor);
                mPaintBox.setAlpha(120);
                canvas.drawRect(leftX, topY, rightX, bottomY, mPaintBox);

                // Corner brackets
                mPaintBoxCorner.setColor(mainColor);
                mPaintBoxCorner.setAlpha(255);
                float cornerLen = Math.min(boxWidth * 0.28f, 22f);

                // Top-Left
                canvas.drawLine(leftX, topY, leftX + cornerLen, topY, mPaintBoxCorner);
                canvas.drawLine(leftX, topY, leftX, topY + cornerLen, mPaintBoxCorner);

                // Top-Right
                canvas.drawLine(rightX, topY, rightX - cornerLen, topY, mPaintBoxCorner);
                canvas.drawLine(rightX, topY, rightX, topY + cornerLen, mPaintBoxCorner);

                // Bottom-Left
                canvas.drawLine(leftX, bottomY, leftX + cornerLen, bottomY, mPaintBoxCorner);
                canvas.drawLine(leftX, bottomY, leftX, bottomY - cornerLen, mPaintBoxCorner);

                // Bottom-Right
                canvas.drawLine(rightX, bottomY, rightX - cornerLen, bottomY, mPaintBoxCorner);
                canvas.drawLine(rightX, bottomY, rightX, bottomY - cornerLen, mPaintBoxCorner);
            }

            // 3. Draw Health Bar
            if (drawHealth) {
                float barWidth = 4.5f;
                float barLeft = leftX - barWidth - 4f;
                float barRight = barLeft + barWidth;

                // Background
                canvas.drawRect(barLeft, topY, barRight, bottomY, mPaintHealthBg);

                // Clamped health 0..100
                float hpPercent = Math.max(0.0f, Math.min(1.0f, health / 100.0f));
                float filledHeight = (bottomY - topY) * hpPercent;
                float barTop = bottomY - filledHeight;

                if (hpPercent > 0.6f) {
                    mPaintHealthFill.setColor(Color.parseColor("#00E676")); // Green
                } else if (hpPercent > 0.25f) {
                    mPaintHealthFill.setColor(Color.parseColor("#FFD600")); // Yellow
                } else {
                    mPaintHealthFill.setColor(Color.parseColor("#FF1744")); // Red
                }
                canvas.drawRect(barLeft, barTop, barRight, bottomY, mPaintHealthFill);
            }

            // 4. Draw Distance & Info Tag
            if (drawDistance) {
                String label = tag + " [" + (int) distance + "m]";
                float textY = bottomY + 22f;

                // Stroke outline first for maximum contrast
                canvas.drawText(label, centerX, textY, mPaintTextStroke);
                // Fill text in main color
                mPaintText.setColor(mainColor);
                canvas.drawText(label, centerX, textY, mPaintText);
            }
        }

        // 60 FPS refresh loop
        postInvalidateDelayed(16);
    }
}

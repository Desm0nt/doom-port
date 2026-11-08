package com.xiaoboluo.d2r;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.PixelFormat;
import android.graphics.PorterDuff;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Process;
import android.os.SystemClock;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import java.util.concurrent.atomic.AtomicBoolean;

// [front] The on-screen pad's own surface, drawn on its own thread ("dm-pad-draw") with a CPU canvas, so the UI thread never draws a frame while
// the game runs and never waits for the GPU (frontend-common; from DOOM Eternal's anrfix b1b9a60, docs/port-kb APP-01). Opt-in per port:
// DoomPadView.attachSurface() (DOOM 2016 and Eternal); a port that never calls it keeps the old in-view drawing, unchanged.
// Why: drawn on the window, every invalidate() of the pad (each touch event, the frame counter twice a second) made the UI thread run
// Choreographer.doFrame -> ViewRootImpl.performDraw -> ThreadedRenderer.draw -> HardwareRenderer.nSyncAndDrawFrame, which waits for HWUI's
// RenderThread. RenderThread renders the window with the GPU through the system GL driver; when our Vulkan work holds the GPU for seconds
// (Eternal nrec4-b1: "Throttling EGL Production: fence ... didn't signal in 3000 ms" on RenderThread and in SurfaceFlinger, "QueueBuffer time
// out ... 9317 ms"), RenderThread waits on its previous frame's fence, the UI thread waits on RenderThread (MIUIScout backtrace:
// HardwareRenderer.nSyncAndDrawFrame inside Choreographer.doFrame, pending message DoomPadView$1 = the frame counter tick), input is not
// consumed and after 5 s the system reports "Input dispatching timed out" and kills the app. On the Switch nothing in the UI path waits for the GPU.
// Now: this SurfaceView sits above the game's SurfaceView and below the window (setZOrderMediaOverlay), so the pad keeps its place in the
// stack (game < pad < pause menu / dialogs). The UI thread only updates the pad's state (touches still go to the DoomPadView on the window, as
// before: input reaches the game exactly as fast) and calls request(); the draw thread takes the pad's lock for the few ms of painting, never
// while it waits for a buffer (lockCanvas can wait for the compositor during such a GPU stall: then the pad image freezes, the touches and the
// game input keep working). The surface the thread draws to is a second Java Surface over the same native window (nativeWrap): SurfaceView's
// own Surface object, whose lock the UI thread takes on surface changes, is never used to draw.
// A frame is drawn only when what the pad shows changed (Painter.paintSignature): a look drag, which moves nothing on the pad, draws nothing.
// Unlike Eternal's first version the draw thread survives the window being detached and attached again (DOOM 2016 swaps the content view for
// its folder / error screens and back): it is quit on detach and started again on attach.
// Debug: stallMs = N makes this thread sleep N ms after taking a buffer (at most once every 15 s), standing in for a GPU stall: the pad freezes,
// input and the UI thread must not (DOOM 2016: intent extra dmPadStallMs / DM_PAD_STALL_MS).
final class PadSurface extends SurfaceView implements SurfaceHolder.Callback {
    interface Painter {
        Object paintLock();
        // Under paintLock(), on the draw thread: a hash of everything the pad draws (equal = the frame on screen still shows it), or NOT_READY.
        long paintSignature();
        void paint(Canvas c);   // under paintLock(), on the draw thread, onto a cleared canvas
    }
    static final long NOT_READY = Long.MIN_VALUE;
    static volatile int stallMs;   // debug: the draw thread sleeps this long holding a buffer (dmPadStallMs / DM_PAD_STALL_MS)
    static native Surface nativeWrap(Surface s);   // jni_android.c: a new Java Surface over the same ANativeWindow

    private final Painter painter;
    private HandlerThread thread; private volatile Handler h;
    private final AtomicBoolean queued = new AtomicBoolean();
    private final Runnable drawTask = this::drawNow;
    private static boolean wrapMissing;
    // draw thread only
    private Surface out; private boolean outOwned, forced; private long shownSig; private int frames; private long lastStall, slowTold, slowPaintTold;

    PadSurface(Context c, Painter p) {
        super(c);
        painter = p;
        setZOrderMediaOverlay(true);
        getHolder().setFormat(PixelFormat.TRANSLUCENT);
        getHolder().addCallback(this);
        startThread();
    }
    private void startThread() {
        if (thread != null && thread.isAlive()) return;
        thread = new HandlerThread("dm-pad-draw", Process.THREAD_PRIORITY_DISPLAY);
        thread.start();
        h = new Handler(thread.getLooper());
    }

    // Any thread: draw once more soon (requests made while one is queued fold into it).
    void request() { Handler hh = h; if (hh != null && queued.compareAndSet(false, true)) { if (!hh.post(drawTask)) queued.set(false); } }

    // ---- SurfaceHolder.Callback (UI thread): hand the surface over and return; nothing here waits for the draw thread
    @Override public void surfaceCreated(SurfaceHolder holder) {}
    @Override public void surfaceChanged(SurfaceHolder holder, int format, int w, int ht) {
        Surface s = holder.getSurface(), wrapped = null;
        if (!wrapMissing) try { wrapped = nativeWrap(s); } catch (UnsatisfiedLinkError e) { wrapMissing = true; android.util.Log.i("dm", "[java] pad surface: no nativeWrap in the library, drawing on SurfaceView's own Surface"); }
        final Surface use = wrapped != null ? wrapped : s; final boolean owned = wrapped != null;
        Handler hh = h; if (hh != null) hh.post(() -> adopt(use, owned, w, ht));
    }
    @Override public void surfaceDestroyed(SurfaceHolder holder) { Handler hh = h; if (hh != null) hh.post(() -> adopt(null, false, 0, 0)); }
    @Override protected void onAttachedToWindow() { super.onAttachedToWindow(); startThread(); }
    @Override protected void onDetachedFromWindow() {
        super.onDetachedFromWindow();
        Handler hh = h; h = null; queued.set(false);
        if (hh != null) hh.post(() -> adopt(null, false, 0, 0));
        if (thread != null) thread.quitSafely();   // the posted adopt(null) still runs: quitSafely finishes what is due
    }

    // ---- draw thread
    private void adopt(Surface s, boolean owned, int w, int ht) {
        if (out != null && out != s && outOwned) try { out.release(); } catch (RuntimeException e) { /* gone already */ }
        out = s; outOwned = owned; forced = true; frames = 0;
        if (s != null) { android.util.Log.i("dm", "[java] pad surface " + w + "x" + ht + " (draw thread dm-pad-draw" + (owned ? "" : ", SurfaceView's Surface") + ")"); drawNow(); }
    }
    private void drawNow() {
        queued.set(false);
        Surface s = out; if (s == null || !s.isValid()) return;
        Object lock = painter.paintLock();
        synchronized (lock) { long sig = painter.paintSignature(); if (sig == NOT_READY || (!forced && sig == shownSig)) return; }
        long t0 = SystemClock.uptimeMillis();
        Canvas c;
        try { c = s.lockCanvas(null); } catch (RuntimeException e) { android.util.Log.i("dm", "[java] pad surface: lockCanvas failed (" + e + ")"); return; }
        long waited = SystemClock.uptimeMillis() - t0;
        if (waited >= 500 && t0 - slowTold >= 5000) { slowTold = t0; android.util.Log.i("dm", "[java] pad draw thread waited " + waited + " ms for a buffer (the compositor / GPU was stalled; the UI thread did not wait)"); }
        int st = stallMs;
        if (st > 0 && (lastStall == 0 || t0 - lastStall >= 15000)) { lastStall = t0; android.util.Log.i("dm", "[java] pad draw thread: test stall " + st + " ms holding a buffer (dmPadStallMs)"); SystemClock.sleep(st); }
        long sig = NOT_READY, p0 = 0, p1 = 0;
        try {
            c.drawColor(0, PorterDuff.Mode.CLEAR);
            synchronized (lock) { p0 = SystemClock.uptimeMillis(); sig = painter.paintSignature(); if (sig != NOT_READY) painter.paint(c); p1 = SystemClock.uptimeMillis(); }
        } finally {
            try { s.unlockCanvasAndPost(c); } catch (RuntimeException e) { android.util.Log.i("dm", "[java] pad surface: post failed (" + e + ")"); sig = NOT_READY; }
        }
        // the UI thread's touch handler waits for this lock while the pad is painted: say so when painting is slow (touch latency)
        if (p1 - p0 >= 8 && p0 - slowPaintTold >= 10000) { slowPaintTold = p0; android.util.Log.i("dm", "[java] pad paint took " + (p1 - p0) + " ms under the pad lock"); }
        if (sig == NOT_READY) return;
        shownSig = sig; forced = false;
        if (frames++ == 0) android.util.Log.i("dm", "[java] pad surface: first frame in " + (SystemClock.uptimeMillis() - t0) + " ms (paint " + (p1 - p0) + " ms)");
    }
}

// base: library
// Sound: attachSound, duration, volume and pan per target, start/stop, onSoundComplete.
// The runner checks that the mixed output is not silent.
class SoundApi {
    static function main(mc:MovieClip):Void {
        var s:Sound = new Sound();
        s.attachSound("beep");
        trace(s.duration + " " + s.getVolume() + " " + s.getPan());
        s.setVolume(50);
        var clipSound:Sound = new Sound(mc);
        clipSound.setVolume(80);
        clipSound.setPan(-30);
        trace(s.getVolume() + " " + clipSound.getVolume() + " " + clipSound.getPan());
        var completes:Number = 0;
        s.onSoundComplete = function() {
            completes++;
            trace("complete " + completes);
        };
        var other:Sound = new Sound(mc);
        other.attachSound("beep");
        other.onSoundComplete = function() { trace("stopped sounds must not complete"); };
        other.start(0, 5);
        other.stop("beep");
        s.start();
        var missing:Sound = new Sound();
        missing.attachSound("nothing");
        trace(missing.duration);
        var frames:Number = 0;
        mc.onEnterFrame = function() {
            frames++;
            if (frames == 8) {
                s.start(0, 2);
            }
            if (frames == 20) {
                trace("done " + completes);
                delete this.onEnterFrame;
            }
        };
    }
}

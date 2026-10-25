// Color (setRGB/getRGB, setTransform/getTransform) and Stage (size, scaleMode).
class ColorStage {
    static function main(mc:MovieClip):Void {
        var c:MovieClip = mc.createEmptyMovieClip("c", 1);
        var col:Color = new Color(c);
        col.setRGB(0x336699);
        trace(col.getRGB().toString(16));
        var t:Object = col.getTransform();
        trace(t.ra + " " + t.rb + " " + t.ga + " " + t.gb + " " + t.aa + " " + t.ab);
        col.setTransform({ra: 50, rb: 10, aa: 100, ab: 0});
        t = col.getTransform();
        trace(t.ra + " " + t.rb + " " + t.ga + " " + t.gb);
        trace(Stage.width + " " + Stage.height + " " + Stage.scaleMode);
        Stage.scaleMode = "noScale";
        trace(Stage.scaleMode);
        Stage.scaleMode = "bogus";
        trace(Stage.scaleMode);
        var l:Object = {};
        Stage.addListener(l);
        trace(Stage["_listeners"].length);
    }
}

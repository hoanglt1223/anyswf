// createTextField, text/htmlText, colours, TextFormat, autoSize and variable binding.
class TextFieldApi {
    static function main(mc:MovieClip):Void {
        mc.createTextField("t", 3, 10, 10, 200, 20);
        var t:Object = mc["t"];
        trace(typeof t + " " + t._name + " " + t.getDepth() + " " + t._x + " " + t._width);
        t.text = "Hello";
        trace(t.text + " " + t.length + " " + t.html + " " + t.type + " " + t.multiline);
        t.textColor = 0xFF0000;
        trace(t.textColor);
        t.border = true;
        t.backgroundColor = 0x00FF00;
        trace(t.border + " " + t.background + " " + t.backgroundColor);
        t.html = true;
        t.htmlText = "<b>Bold</b> &amp; text";
        trace(t.text);
        var f = new TextFormat("_sans", 20, 0x0000FF);
        trace(f.size + " " + f.color + " " + f.bold + " " + f.align);
        t.setTextFormat(f);
        var g = t.getTextFormat();
        trace(g.size + " " + g.color + " " + g.align);
        t.autoSize = "left";
        trace(t.autoSize + " " + (t._width < 200));
        _root.score = 5;
        t.html = false;
        t.variable = "_root.score";
        trace(t.variable);
        var frames:Number = 0;
        mc.onEnterFrame = function() {
            frames++;
            if (frames == 1) {
                trace("bound " + t.text);
                _root.score = 42;
            } else if (frames == 2) {
                trace("bound " + t.text);
                t.text = "7";
                trace("score " + _root.score);
                t.removeTextField();
                trace("removed " + (mc["t"] == undefined));
                delete this.onEnterFrame;
            }
        };
    }
}

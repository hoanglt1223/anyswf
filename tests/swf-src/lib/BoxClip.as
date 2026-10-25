// Class linked to the library's "box" symbol by MovieClipApi.
class BoxClip extends MovieClip {
    var tag:String;
    function BoxClip() {
        trace("ctor " + this._name + " " + tag);
    }
    function onLoad():Void {
        trace("onLoad " + this._name);
    }
    function size():String {
        return this._width + "x" + this._height;
    }
}

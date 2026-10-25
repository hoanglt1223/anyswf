// SharedObject.getLocal/flush across two runs of the movie (the runner reuses the save
// directory): the first run writes, the second reads what was saved.
class SharedObjectApi {
    static function main(mc:MovieClip):Void {
        var so:SharedObject = SharedObject.getLocal("test-save");
        var data:Object = so.data;
        if (data.count == undefined) {
            trace("fresh " + so.getSize());
            data.count = 1;
            data.name = "abc";
            data.list = [1, "two", true];
            data.nested = {x: 1.5, y: null};
            data.nested.self = data.nested;
            data.when = new Date(1000);
            data.fn = function() {};
            trace(so.flush());
        } else {
            trace("loaded " + data.count + " " + data.name + " " + data.list.length + " " + data.list[1] + " " +
                  data.list[2] + " " + data.nested.x + " " + data.nested.y + " " +
                  (data.nested.self == data.nested) + " " + data.when.getTime() + " " + (data.fn == undefined));
            data.count++;
        }
        trace(SharedObject.getLocal("test-save") == so);
        trace(SharedObject.getLocal("bad name") == null);
        var other:SharedObject = SharedObject.getLocal("other");
        trace(other.data.count == undefined);
    }
}

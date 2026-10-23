class ObjectsStrings {
    static function main(mc:MovieClip):Void {
        var o = {x: 1};
        o.y = 2;
        trace(o.x + o.y);
        trace(o.missing);
        trace(o);

        var arr = [3, 1, 2];
        arr.sort();
        trace(arr);
        trace(arr.length);
        arr.push(4);
        trace(arr.join("-"));

        var s = "Hello";
        trace(s.toUpperCase());
        trace(s.charAt(1));
        trace(s.indexOf("l"));
        trace(s.substr(1, 3));
        trace("a,b,c".split(",").length);

        trace(typeof 1);
        trace(typeof "s");
        trace(typeof o);
        trace(typeof undefined);
        trace(typeof null);
        trace(typeof main);
    }
}

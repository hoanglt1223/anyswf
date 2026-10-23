class TraceBasics {
    static function main(mc:MovieClip):Void {
        trace("hello");
        trace(42);
        trace(1.5);
        trace(-0.25);
        trace(true);
        trace(false);
        trace(null);
        trace(undefined);
        var zero = 0;
        trace(1 / zero);
        trace(-1 / zero);
        trace(zero / zero);
        trace(123456789012);
        trace(1e21);
        trace("a" + "b" + 1 + 2);
    }
}

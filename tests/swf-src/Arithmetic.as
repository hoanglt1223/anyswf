class Arithmetic {
    static function main(mc:MovieClip):Void {
        var a = "5";
        var b = 3;
        var u;
        trace(a + b);
        trace(a * 2);
        trace(a - b);
        trace(7 % 3);
        trace(-7 % 3);
        trace(10 / 4);
        trace(0.1 + 0.2);
        trace(1 + u);
        trace(Math.floor(-2.5));
        trace(parseInt("ff", 16));
        trace(5 == "5");
        trace(5 === "5");
        trace(null == undefined);
        trace("abc" < "abd");
        trace(7 & 3);
        trace(1 << 4);
        trace(-16 >> 2);
    }
}

class ControlFlow {
    static function main(mc:MovieClip):Void {
        var sum = 0;
        for (var i = 0; i < 5; i++) {
            sum += i;
        }
        trace(sum);

        var n = 3;
        while (n > 0) {
            trace(n);
            n--;
        }

        var k = 0;
        do {
            k++;
        } while (k < 10);
        trace(k);

        for (var j = 0; j < 6; j++) {
            if (j % 2 == 0) continue;
            trace(j);
        }

        var s = "b";
        switch (s) {
            case "a":
                trace("case a");
                break;
            case "b":
                trace("case b");
            case "c":
                trace("fell into c");
                break;
            default:
                trace("default");
        }

        trace(sum > 5 ? "big" : "small");
    }
}

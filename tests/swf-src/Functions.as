class Functions {
    static function fact(n:Number):Number {
        return n <= 1 ? 1 : n * fact(n - 1);
    }

    static function makeCounter():Function {
        var count = 0;
        return function() {
            count++;
            return count;
        };
    }

    static function countArgs():Number {
        return arguments.length;
    }

    static function second(a, b) {
        return b;
    }

    static function main(mc:MovieClip):Void {
        trace(fact(5));
        var next = makeCounter();
        trace(next());
        trace(next());
        var other = makeCounter();
        trace(other());
        trace(countArgs(1, "two", 3));
        trace(second(1));
        var o = {name: "obj"};
        var getName = function() {
            return this.name;
        };
        trace(getName.call(o));
        trace(getName.apply(o, []));
    }
}

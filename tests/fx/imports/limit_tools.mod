mod LimitTools {
    export const Limit: Int = 3;
    export fn cap(x: Int) -> Int { if x > Limit { ret Limit; } ret x; }
}

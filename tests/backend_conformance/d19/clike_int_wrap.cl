// D19 probe (report only), the CLike side: components/clike/tests/Int64.cl
// pins a store truncation (5000000000 into an int prints 705032704), which
// widening must leave alone. pin_gate prints these lines next to the ones
// recorded in clike_int_wrap.today and flags any that moved.
int main() {
    int i;
    long long w;
    i = 5000000000;
    printf("store_truncation=%lld\n", i);
    i = 2147483647;
    i = i + 1;
    printf("stored_int=%lld\n", i);
    w = 2147483647;
    w = w + 1;
    printf("stored_long_long=%lld\n", w);
    printf("unstored_literal=%lld\n", 2147483647 + 1);
    i = 2147483647;
    printf("unstored_from_int=%lld\n", i + 1);
    return 0;
}

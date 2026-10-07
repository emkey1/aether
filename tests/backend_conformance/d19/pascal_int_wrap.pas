program PascalIntWrap;
{ D19 probe (report only). Settles whether INT64 widening (W5-04) can ship
  ungated: it can, unless a Pascal golden depends on 32-bit wrap of an
  unstored intermediate. pscal Pascal has no predeclared MaxInt, so the
  probe declares the 32-bit maximum itself. pin_gate prints these lines next
  to the ones recorded in pascal_int_wrap.today and flags any that moved. }
const
  MaxInt32 = 2147483647;
var
  i: integer;
  k: longint;
  j: int64;
begin
  i := MaxInt32;
  i := i + 1;
  writeln('stored_integer=', i);
  k := MaxInt32;
  k := k + 1;
  writeln('stored_longint=', k);
  j := MaxInt32;
  j := j + 1;
  writeln('stored_int64=', j);
  writeln('unstored_literal=', MaxInt32 + 1);
  i := MaxInt32;
  writeln('unstored_from_integer=', i + 1);
end.

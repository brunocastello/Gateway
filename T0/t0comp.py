#!/usr/bin/env python3
"""
T0 compiler - Python 3 port of T0Comp.cs (C# / .NET Framework).

Original: Copyright (c) 2016 Thomas Pornin <pornin@bolet.org> (MIT license).
This port aims for full behavioural parity with the C# version while only
requiring a standard Python 3 interpreter (no .NET).

Usage:
    python3 t0comp.py [ options... ] file...
options:
   -o file    use 'file' as base for output file name (default: 't0out')
   -r name    use 'name' as base for run function (default: same as output)
   -m name[,name...]
              define entry point(s)
   -nf        disable flow analysis
"""

import os
import sys

INT32_MIN = -2147483648
INT32_MAX = 2147483647
UINT32_MAX = 0xFFFFFFFF


# --------------------------------------------------------------------------
# 32-bit helpers (emulate C# unchecked int/uint semantics)
# --------------------------------------------------------------------------

def to_u32(x):
    return x & 0xFFFFFFFF


def to_i32(x):
    x &= 0xFFFFFFFF
    return x - 0x100000000 if x >= 0x80000000 else x


def wrap_i32(x):
    return to_i32(x)


def c_div(a, b):
    # C# integer division: truncation toward zero. Caller ensures b != 0.
    # a, b are signed 32-bit.
    q = abs(a) // abs(b)
    if (a < 0) != (b < 0):
        q = -q
    return to_i32(q)


def c_mod(a, b):
    return to_i32(a - c_div(a, b) * b)


def c_udiv(a, b):
    au = to_u32(a)
    bu = to_u32(b)
    return to_i32(au // bu)


def c_umod(a, b):
    au = to_u32(a)
    bu = to_u32(b)
    return to_i32(au % bu)


# --------------------------------------------------------------------------
# SType
# --------------------------------------------------------------------------

class SType:
    __slots__ = ("din", "dout")

    def __init__(self, din, dout):
        self.din = -1 if din < 0 else din
        self.dout = -1 if dout < 0 else dout

    @property
    def data_in(self):
        return self.din

    @property
    def data_out(self):
        return self.dout

    # aliases matching C# names
    @property
    def DataIn(self):
        return self.din

    @property
    def DataOut(self):
        return self.dout

    @property
    def is_known(self):
        return self.din >= 0

    @property
    def IsKnown(self):
        return self.din >= 0

    @property
    def no_exit(self):
        return self.din >= 0 and self.dout < 0

    @property
    def NoExit(self):
        return self.din >= 0 and self.dout < 0

    def __eq__(self, other):
        return isinstance(other, SType) and self.din == other.din and self.dout == other.dout

    def __ne__(self, other):
        return not self.__eq__(other)

    def __hash__(self):
        return self.din * 31 + self.dout * 17

    def __str__(self):
        if not self.is_known:
            return "UNKNOWN"
        elif self.no_exit:
            return "in:%d,noexit" % self.din
        else:
            return "in:%d,out:%d" % (self.din, self.dout)

    def is_sub_of(self, s):
        if not self.is_known or not s.is_known:
            return False
        if self.din > s.din:
            return False
        if self.no_exit:
            return True
        if s.no_exit:
            return False
        return (self.din - self.dout) == (s.din - s.dout)

    def IsSubOf(self, s):
        return self.is_sub_of(s)


SType.UNKNOWN = SType(-1, -1)
SType.BLANK = SType(0, 0)


# --------------------------------------------------------------------------
# TValue
# --------------------------------------------------------------------------

class TValue:
    __slots__ = ("x", "ptr")

    def __init__(self, x=0, ptr=None):
        # x is always stored as signed 32-bit
        if isinstance(x, bool):
            self.x = -1 if x else 0
        else:
            self.x = to_i32(int(x))
        self.ptr = ptr

    @staticmethod
    def from_bool(b):
        return TValue(-1 if b else 0)

    @staticmethod
    def from_int(x):
        return TValue(to_i32(x))

    @staticmethod
    def from_uint(x):
        return TValue(to_i32(to_u32(x)))

    def to_bool(self):
        if self.ptr is None:
            return self.x != 0
        else:
            return self.ptr.to_bool(self)

    def to_int(self):
        if self.ptr is None:
            return self.x
        raise Exception("not an integer: " + self.to_string())

    def to_uint(self):
        return to_u32(self.to_int())

    @property
    def Bool(self):
        return self.to_bool()

    @property
    def Int(self):
        return self.to_int()

    @property
    def UInt(self):
        return self.to_uint()

    def to_string(self):
        if self.ptr is None:
            return "%d" % self.x
        else:
            return self.ptr.to_string(self)

    def __str__(self):
        return self.to_string()

    def execute(self, ctx, cpu):
        self.to_xt().execute(ctx, cpu)

    def Execute(self, ctx, cpu):
        self.execute(ctx, cpu)

    def to_xt(self):
        from_xt = self.ptr if isinstance(self.ptr, TPointerXT) else None
        if from_xt is None:
            raise Exception("value is not an xt: " + self.to_string())
        return from_xt

    def ToXT(self):
        return self.to_xt()

    def equals(self, v):
        if self.x != v.x:
            return False
        if self.ptr is v.ptr:
            return True
        if self.ptr is None or v.ptr is None:
            return False
        return self.ptr.equals(v.ptr)

    def Equals(self, v):
        return self.equals(v)


def tv_int(v):
    return v.to_int()


def tv_uint(v):
    return v.to_uint()


def tv_bool(v):
    return v.to_bool()


# --------------------------------------------------------------------------
# TPointer hierarchy
# --------------------------------------------------------------------------

class TPointerBase:
    def to_bool(self, vp):
        return True

    def ToBool(self, vp):
        return self.to_bool(vp)

    def execute(self, ctx, cpu):
        raise Exception("value is not an xt: " + self.to_string(None))

    def Execute(self, ctx, cpu):
        self.execute(ctx, cpu)

    def to_string(self, vp):
        x = vp.x if vp is not None else 0
        return "%s+%d" % (type(self).__name__, x)

    def ToString(self, vp):
        return self.to_string(vp)

    def equals(self, tp):
        return self is tp

    def Equals(self, tp):
        return self.equals(tp)


class TPointerBlob(TPointerBase):
    def __init__(self, blob_or_owner, s=None):
        if isinstance(blob_or_owner, ConstData):
            self.blob = blob_or_owner
        else:
            # owner, string
            owner = blob_or_owner
            self.blob = ConstData(owner)
            self.blob.add_string(s)

    @property
    def Blob(self):
        return self.blob

    def to_string(self, vp):
        return self.blob.to_string(vp.x)

    def equals(self, tp):
        return isinstance(tp, TPointerBlob) and self.blob is tp.blob


class TPointerExpr(TPointerBase):
    def __init__(self, expr, minv, maxv):
        self.expr = expr
        self.min = minv
        self.max = maxv

    def to_bool(self, vp):
        raise Exception("Cannot evaluate C-expr at compile time")

    def to_string(self, vp):
        return self.to_c_expr(vp.x)

    def to_c_expr(self, off):
        if off == 0:
            return self.expr
        elif off > 0:
            return "(uint32_t)(%s) + %d" % (self.expr, off)
        else:
            return "(uint32_t)(%s) - %d" % (self.expr, -off)

    def ToCExpr(self, off):
        return self.to_c_expr(off)

    def get_max_bit_length(self, off):
        rmin = self.min + off
        rmax = self.max + off
        num_bits = 1
        if rmin < 0:
            num_bits = max(num_bits, _bit_length(rmin))
        if rmax > 0:
            num_bits = max(num_bits, _bit_length(rmax))
        return min(num_bits, 32)

    def GetMaxBitLength(self, off):
        return self.get_max_bit_length(off)


def _bit_length(v):
    num = 1
    if v < 0:
        while v != -1:
            num += 1
            v >>= 1
    else:
        while v != 0:
            num += 1
            v >>= 1
    return num


class TPointerNull(TPointerBase):
    def to_bool(self, vp):
        return False

    def to_string(self, vp):
        return "null"

    def equals(self, tp):
        return isinstance(tp, TPointerNull)


class TPointerXT(TPointerBase):
    def __init__(self, name_or_target):
        if isinstance(name_or_target, str):
            self.name = name_or_target
            self.target = None
        else:
            self.name = name_or_target.name
            self.target = name_or_target

    @property
    def Name(self):
        return self.name

    @property
    def Target(self):
        return self.target

    def resolve(self, ctx):
        if self.target is None:
            self.target = ctx.lookup(self.name)

    def Resolve(self, ctx):
        self.resolve(ctx)

    def execute(self, ctx, cpu):
        self.resolve(ctx)
        self.target.run(cpu)

    def to_string(self, vp):
        return "<'%s>" % self.name

    def equals(self, tp):
        return isinstance(tp, TPointerXT) and self.name == tp.name


# --------------------------------------------------------------------------
# ConstData
# --------------------------------------------------------------------------

class ConstData:
    def __init__(self, ctx):
        self.id = ctx.next_blob_id()
        self.address = 0
        self._buf = bytearray(4)
        self._len = 0

    @property
    def ID(self):
        return self.id

    @property
    def Address(self):
        return self.address

    @Address.setter
    def Address(self, v):
        self.address = v

    @property
    def Length(self):
        return self._len

    def _expand(self, elen):
        tlen = self._len + elen
        if tlen > len(self._buf):
            nlen = max(len(self._buf) * 2, tlen)
            nb = bytearray(nlen)
            nb[0:self._len] = self._buf[0:self._len]
            self._buf = nb

    def add8(self, b):
        self._expand(1)
        self._buf[self._len] = b & 0xFF
        self._len += 1

    def Add8(self, b):
        self.add8(b)

    def add16(self, x):
        self._expand(2)
        self._buf[self._len] = (x >> 8) & 0xFF
        self._buf[self._len + 1] = x & 0xFF
        self._len += 2

    def add24(self, x):
        self._expand(3)
        self._buf[self._len] = (x >> 16) & 0xFF
        self._buf[self._len + 1] = (x >> 8) & 0xFF
        self._buf[self._len + 2] = x & 0xFF
        self._len += 3

    def add32(self, x):
        self._expand(4)
        self._buf[self._len] = (x >> 24) & 0xFF
        self._buf[self._len + 1] = (x >> 16) & 0xFF
        self._buf[self._len + 2] = (x >> 8) & 0xFF
        self._buf[self._len + 3] = x & 0xFF
        self._len += 4

    def add_string(self, s):
        sd = s.encode("utf-8")
        self._expand(len(sd) + 1)
        self._buf[self._len:self._len + len(sd)] = sd
        self._buf[self._len + len(sd)] = 0
        self._len += len(sd) + 1

    def AddString(self, s):
        self.add_string(s)

    def _check_index(self, off, dlen):
        if off < 0 or off > (self._len - dlen):
            raise IndexError("ConstData index out of range")

    def set8(self, off, v):
        self._check_index(off, 1)
        self._buf[off] = v & 0xFF

    def Set8(self, off, v):
        self.set8(off, v)

    def read8(self, off):
        self._check_index(off, 1)
        return self._buf[off]

    def Read8(self, off):
        return self.read8(off)

    def read16(self, off):
        self._check_index(off, 2)
        return (self._buf[off] << 8) | self._buf[off + 1]

    def read24(self, off):
        self._check_index(off, 3)
        return (self._buf[off] << 16) | (self._buf[off + 1] << 8) | self._buf[off + 2]

    def read32(self, off):
        self._check_index(off, 4)
        return to_i32((self._buf[off] << 24) | (self._buf[off + 1] << 16) | (self._buf[off + 2] << 8) | self._buf[off + 3])

    def to_string(self, off):
        sb = []
        while True:
            off, x = self._decode_utf8(off)
            if x == 0:
                return "".join(sb)
            sb.append(chr(x))

    def ToString(self, off=None):
        if off is None:
            return super().__str__()
        return self.to_string(off)

    def _decode_utf8(self, off):
        buf = self._buf
        ln = self._len
        if off >= ln:
            raise IndexError("ConstData index out of range")
        x = buf[off]
        off += 1
        if x < 0xC0 or x > 0xF7:
            return off, x
        if x >= 0xF0:
            elen = 3
            acc = x & 0x07
        elif x >= 0xE0:
            elen = 2
            acc = x & 0x0F
        else:
            elen = 1
            acc = x & 0x1F
        if off + elen > ln:
            return off - 1 + 1, x  # return single byte value (off already advanced by 1)
            # Note: original returns x without consuming extra bytes.
        for i in range(elen):
            y = buf[off + i]
            if y < 0x80 or y >= 0xC0:
                return off, x
            acc = (acc << 6) + (y & 0x3F)
        if acc > 0x10FFFF:
            return off, x
        off += elen
        return off, acc

    def encode(self, bw):
        for i in range(self._len):
            bw.append_byte(self._buf[i])

    def Encode(self, bw):
        self.encode(bw)


# --------------------------------------------------------------------------
# BlobWriter
# --------------------------------------------------------------------------

class BlobWriter:
    def __init__(self, w, max_line_len, indent):
        self.w = w
        self.max_line_len = max_line_len
        self.indent = indent
        self.line_len = -1

    def _do_nl(self):
        self.w.write("\n")
        for _ in range(self.indent):
            self.w.write("\t")
        self.line_len = self.indent << 3

    def append_byte(self, b):
        if self.line_len < 0:
            self._do_nl()
        else:
            self.w.write(",")
            self.line_len += 1
            if (self.line_len + 5) > self.max_line_len:
                self._do_nl()
            else:
                self.w.write(" ")
                self.line_len += 1
        self.w.write("0x%02X" % (b & 0xFF))
        self.line_len += 4

    def Append(self, b_or_expr):
        if isinstance(b_or_expr, int):
            self.append_byte(b_or_expr)
        else:
            self.append_expr(b_or_expr)

    def append_expr(self, expr):
        if self.line_len < 0:
            self._do_nl()
        else:
            self.w.write(",")
            self.line_len += 1
            if (self.line_len + 1 + len(expr)) > self.max_line_len:
                self._do_nl()
            else:
                self.w.write(" ")
                self.line_len += 1
        self.w.write("%s" % expr)
        self.line_len += len(expr)


# --------------------------------------------------------------------------
# CodeElement
# --------------------------------------------------------------------------

def encode_7e_unsigned(val, bw):
    val = to_u32(val)
    ln = 1
    w = val
    while w >= 0x80:
        ln += 1
        w >>= 7
    if bw is not None:
        for k in range((ln - 1) * 7, -1, -7):
            x = (val >> k) & 0x7F
            if k > 0:
                x |= 0x80
            bw.append_byte(x)
    return ln


def encode_7e_signed(val, bw):
    val = to_i32(val)
    ln = 1
    if val < 0:
        w = val
        while w < -0x40:
            ln += 1
            w >>= 7
    else:
        w = val
        while w >= 0x40:
            ln += 1
            w >>= 7
    if bw is not None:
        for k in range((ln - 1) * 7, -1, -7):
            x = (val >> k) & 0x7F
            if k > 0:
                x |= 0x80
            bw.append_byte(x)
    return ln


def encode_one_byte(val, bw):
    v = to_u32(val)
    if v > 255:
        raise Exception("Cannot encode '%d' over one byte" % v)
    bw.append_byte(v)
    return 1


class CodeElement:
    def __init__(self):
        self.address = -1
        self.last_length = 0

    @property
    def Address(self):
        return self.address

    @Address.setter
    def Address(self, v):
        self.address = v

    @property
    def LastLength(self):
        return self.last_length

    @LastLength.setter
    def LastLength(self, v):
        self.last_length = v

    def set_jump_target(self, target):
        raise Exception("Code element accepts no target")

    def SetJumpTarget(self, target):
        self.set_jump_target(target)

    def get_length(self, one_byte_code):
        raise NotImplementedError()

    def GetLength(self, one_byte_code):
        return self.get_length(one_byte_code)

    def encode(self, bw, one_byte_code):
        raise NotImplementedError()

    def Encode(self, bw, one_byte_code):
        return self.encode(bw, one_byte_code)

    @staticmethod
    def EncodeOneByte(val, bw):
        return encode_one_byte(val, bw)

    @staticmethod
    def Encode7EUnsigned(val, bw):
        return encode_7e_unsigned(val, bw)

    @staticmethod
    def Encode7ESigned(val, bw):
        return encode_7e_signed(val, bw)


class CodeElementUInt(CodeElement):
    def __init__(self, val):
        super().__init__()
        self.val = to_u32(val)

    def get_length(self, one_byte_code):
        return 1 if one_byte_code else encode_7e_unsigned(self.val, None)

    def encode(self, bw, one_byte_code):
        if one_byte_code:
            return encode_one_byte(self.val, bw)
        else:
            return encode_7e_unsigned(self.val, bw)


class CodeElementJump(CodeElement):
    def __init__(self, jump_type):
        super().__init__()
        self.jump_type = to_u32(jump_type)
        self.target = None

    def get_length(self, one_byte_code):
        ln = 1 if one_byte_code else encode_7e_unsigned(self.jump_type, None)
        joff = self._jump_off()
        if joff == INT32_MIN:
            ln += 1
        else:
            ln += encode_7e_signed(joff, None)
        return ln

    def set_jump_target(self, target):
        self.target = target

    def _jump_off(self):
        if self.target is None or self.address < 0 or self.target.address < 0:
            return INT32_MIN
        else:
            return self.target.address - (self.address + self.last_length)

    def encode(self, bw, one_byte_code):
        if bw is None:
            return self.get_length(one_byte_code)
        if one_byte_code:
            ln = encode_one_byte(self.jump_type, bw)
        else:
            ln = encode_7e_unsigned(self.jump_type, bw)
        joff = self._jump_off()
        if joff == INT32_MIN:
            raise Exception("Unresolved addresses")
        return ln + encode_7e_signed(joff, bw)


class CodeElementUIntInt(CodeElement):
    def __init__(self, val1, val2):
        super().__init__()
        self.val1 = to_u32(val1)
        self.val2 = to_i32(val2)

    def get_length(self, one_byte_code):
        return (1 if one_byte_code else encode_7e_unsigned(self.val1, None)) + encode_7e_signed(self.val2, None)

    def encode(self, bw, one_byte_code):
        if one_byte_code:
            ln = encode_one_byte(self.val1, bw)
        else:
            ln = encode_7e_unsigned(self.val1, bw)
        ln += encode_7e_signed(self.val2, bw)
        return ln


class CodeElementUIntUInt(CodeElement):
    def __init__(self, val1, val2):
        super().__init__()
        self.val1 = to_u32(val1)
        self.val2 = to_u32(val2)

    def get_length(self, one_byte_code):
        return (1 if one_byte_code else encode_7e_unsigned(self.val1, None)) + encode_7e_unsigned(self.val2, None)

    def encode(self, bw, one_byte_code):
        if one_byte_code:
            ln = encode_one_byte(self.val1, bw)
        else:
            ln = encode_7e_unsigned(self.val1, bw)
        ln += encode_7e_unsigned(self.val2, bw)
        return ln


class CodeElementUIntExpr(CodeElement):
    def __init__(self, val, cx, off):
        super().__init__()
        self.val = to_u32(val)
        self.cx = cx
        self.off = to_i32(off)

    def get_length(self, one_byte_code):
        ln = 1 if one_byte_code else encode_7e_unsigned(self.val, None)
        return ln + (self.cx.get_max_bit_length(self.off) + 6) // 7

    def encode(self, bw, one_byte_code):
        if one_byte_code:
            len1 = encode_one_byte(self.val, bw)
        else:
            len1 = encode_7e_unsigned(self.val, bw)
        len2 = (self.cx.get_max_bit_length(self.off) + 6) // 7
        bw.append_expr("T0_INT%d(%s)" % (len2, self.cx.to_c_expr(self.off)))
        return len1 + len2


# --------------------------------------------------------------------------
# CPU
# --------------------------------------------------------------------------

class _Frame:
    __slots__ = ("upper", "saved_ip_buf", "saved_ip_off", "locals")

    def __init__(self, upper, num_locals):
        self.upper = upper
        self.saved_ip_buf = None
        self.saved_ip_off = 0
        self.locals = [TValue(0) for _ in range(num_locals)]


class CPU:
    def __init__(self):
        self.ip_buf = None
        self.ip_off = 0
        self._stack = []
        self._rsp = None

    def enter(self, code, num_locals):
        f = _Frame(self._rsp, num_locals)
        self._rsp = f
        f.saved_ip_buf = self.ip_buf
        f.saved_ip_off = self.ip_off
        self.ip_buf = code
        self.ip_off = 0

    def Enter(self, code, num_locals):
        self.enter(code, num_locals)

    def exit(self):
        self.ip_buf = self._rsp.saved_ip_buf
        self.ip_off = self._rsp.saved_ip_off
        self._rsp = self._rsp.upper

    def Exit(self):
        self.exit()

    @property
    def Depth(self):
        return len(self._stack)

    @property
    def depth(self):
        return len(self._stack)

    def pop(self):
        return self._stack.pop()

    def Pop(self):
        return self.pop()

    def push(self, v):
        if not isinstance(v, TValue):
            # allow int/uint/bool passthrough
            if isinstance(v, bool):
                v = TValue.from_bool(v)
            elif isinstance(v, int):
                v = TValue.from_int(v)
            else:
                raise Exception("cannot push value of type %s" % type(v).__name__)
        self._stack.append(v)

    def Push(self, v):
        self.push(v)

    def peek(self, depth):
        return self._stack[len(self._stack) - 1 - depth]

    def Peek(self, depth):
        return self.peek(depth)

    def rot(self, depth):
        # move value at depth to top
        idx = len(self._stack) - 1 - depth
        v = self._stack.pop(idx)
        self._stack.append(v)

    def Rot(self, depth):
        self.rot(depth)

    def nrot(self, depth):
        v = self._stack.pop()
        idx = len(self._stack) - depth
        self._stack.insert(idx, v)

    def NRot(self, depth):
        self.nrot(depth)

    def get_local(self, num):
        return self._rsp.locals[num]

    def GetLocal(self, num):
        return self.get_local(num)

    def put_local(self, num, v):
        self._rsp.locals[num] = v

    def PutLocal(self, num, v):
        self.put_local(num, v)


# --------------------------------------------------------------------------
# Opcode
# --------------------------------------------------------------------------

class Opcode:
    def run(self, cpu):
        raise NotImplementedError()

    def Run(self, cpu):
        self.run(cpu)

    def resolve_target(self, target):
        raise Exception("Not a call opcode")

    def ResolveTarget(self, target):
        self.resolve_target(target)

    def resolve_jump(self, disp):
        raise Exception("Not a jump opcode")

    def ResolveJump(self, disp):
        self.resolve_jump(disp)

    def get_reference(self, ctx):
        return None

    def GetReference(self, ctx):
        return self.get_reference(ctx)

    def get_data_block(self, ctx):
        return None

    def GetDataBlock(self, ctx):
        return self.get_data_block(ctx)

    @property
    def may_fall_through(self):
        return True

    @property
    def MayFallThrough(self):
        return self.may_fall_through

    @property
    def jump_disp(self):
        return 0

    @property
    def JumpDisp(self):
        return self.jump_disp

    @property
    def stack_action(self):
        return 0

    @property
    def StackAction(self):
        return self.stack_action

    def to_code_element(self):
        raise NotImplementedError()

    def ToCodeElement(self):
        return self.to_code_element()

    def fix_up(self, gcode, off):
        pass

    def FixUp(self, gcode, off):
        self.fix_up(gcode, off)


class OpcodeCall(Opcode):
    def __init__(self, target=None):
        self.target = target

    def resolve_target(self, target):
        if self.target is not None:
            raise Exception("Opcode already resolved")
        self.target = target

    def run(self, cpu):
        self.target.run(cpu)

    def get_reference(self, ctx):
        if self.target is None:
            raise Exception("Unresolved call target")
        return self.target

    def to_code_element(self):
        return CodeElementUInt(to_u32(self.target.slot))

    def __str__(self):
        return "call " + ("UNRESOLVED" if self.target is None else self.target.name)


class OpcodeConst(Opcode):
    def __init__(self, val):
        self.val = val

    def run(self, cpu):
        cpu.push(self.val)

    def get_reference(self, ctx):
        if isinstance(self.val.ptr, TPointerXT):
            xt = self.val.ptr
            xt.resolve(ctx)
            return xt.target
        return None

    def get_data_block(self, ctx):
        if isinstance(self.val.ptr, TPointerBlob):
            return self.val.ptr.blob
        return None

    def to_code_element(self):
        if self.val.ptr is None:
            return CodeElementUIntInt(1, self.val.to_int())
        if isinstance(self.val.ptr, TPointerXT):
            xt = self.val.ptr
            if self.val.x != 0:
                raise Exception("Cannot compile XT: non-zero offset")
            return CodeElementUIntInt(1, to_i32(xt.target.slot))
        if isinstance(self.val.ptr, TPointerBlob):
            bp = self.val.ptr
            return CodeElementUIntInt(1, to_i32(self.val.x + bp.blob.address))
        if isinstance(self.val.ptr, TPointerExpr):
            cx = self.val.ptr
            return CodeElementUIntExpr(1, cx, self.val.x)
        raise Exception("Cannot embed constant (type = %s)" % type(self.val.ptr).__name__)

    @property
    def stack_action(self):
        return 1

    def __str__(self):
        return "const " + self.val.to_string()


class OpcodeJump(Opcode):
    def __init__(self, disp=INT32_MIN):
        self.disp = disp

    @property
    def jump_disp(self):
        return self.disp

    def run(self, cpu):
        cpu.ip_off += self.disp

    def resolve_jump(self, disp):
        if self.disp != INT32_MIN:
            raise Exception("Jump already resolved")
        self.disp = disp

    def fix_up(self, gcode, off):
        gcode[off].set_jump_target(gcode[off + 1 + self.disp])


class OpcodeJumpIf(OpcodeJump):
    def run(self, cpu):
        v = cpu.pop()
        if v.to_bool():
            cpu.ip_off += self.disp

    @property
    def stack_action(self):
        return -1

    def to_code_element(self):
        return CodeElementJump(5)

    def __str__(self):
        if self.jump_disp == INT32_MIN:
            return "jumpif UNRESOLVED"
        else:
            return "jumpif disp=%d" % self.jump_disp


class OpcodeJumpIfNot(OpcodeJump):
    def run(self, cpu):
        v = cpu.pop()
        if not v.to_bool():
            cpu.ip_off += self.disp

    @property
    def stack_action(self):
        return -1

    def to_code_element(self):
        return CodeElementJump(6)

    def __str__(self):
        if self.jump_disp == INT32_MIN:
            return "jumpifnot UNRESOLVED"
        else:
            return "jumpifnot disp=%d" % self.jump_disp


class OpcodeJumpUncond(OpcodeJump):
    @property
    def may_fall_through(self):
        return self.disp == 0

    def to_code_element(self):
        return CodeElementJump(4)

    def __str__(self):
        if self.jump_disp == INT32_MIN:
            return "jump UNRESOLVED"
        else:
            return "jump disp=%d" % self.jump_disp


class OpcodeGetLocal(Opcode):
    def __init__(self, num):
        self.num = num

    def run(self, cpu):
        cpu.push(cpu.get_local(self.num))

    def to_code_element(self):
        return CodeElementUIntUInt(2, to_u32(self.num))

    @property
    def stack_action(self):
        return 1

    def __str__(self):
        return "getlocal %d" % self.num


class OpcodePutLocal(Opcode):
    def __init__(self, num):
        self.num = num

    def run(self, cpu):
        cpu.put_local(self.num, cpu.pop())

    def to_code_element(self):
        return CodeElementUIntUInt(3, to_u32(self.num))

    @property
    def stack_action(self):
        return -1

    def __str__(self):
        return "putlocal %d" % self.num


class OpcodeRet(Opcode):
    def run(self, cpu):
        cpu.exit()

    @property
    def may_fall_through(self):
        return False

    def to_code_element(self):
        return CodeElementUInt(0)

    def __str__(self):
        return "ret"


# --------------------------------------------------------------------------
# Word
# --------------------------------------------------------------------------

class Word:
    def __init__(self, owner, name):
        self.tc = owner
        self.TC = owner
        self.name = name
        self.immediate = False
        self.Immediate = False
        self.slot = 0
        self.Slot = 0
        self.stack_effect = SType.UNKNOWN

    @property
    def Name(self):
        return self.name

    @property
    def Slot(self):
        return self.slot

    @Slot.setter
    def Slot(self, v):
        self.slot = v

    @property
    def StackEffect(self):
        return self.stack_effect

    @StackEffect.setter
    def StackEffect(self, v):
        self.stack_effect = v

    @property
    def Immediate(self):
        return self.immediate

    @Immediate.setter
    def Immediate(self, v):
        self.immediate = v

    def resolve(self):
        pass

    def Resolve(self):
        self.resolve()

    def run(self, cpu):
        raise Exception("cannot run '%s' at compile-time" % self.name)

    def Run(self, cpu):
        self.run(cpu)

    @property
    def CCode(self):
        return None

    def get_references(self):
        return []

    def GetReferences(self):
        return self.get_references()

    def get_data_blocks(self):
        return []

    def GetDataBlocks(self):
        return self.get_data_blocks()

    def generate_code_elements(self, dst):
        raise Exception("Word does not yield code elements")

    def GenerateCodeElements(self, dst):
        self.generate_code_elements(dst)

    def analyse_flow(self):
        pass

    def AnalyseFlow(self):
        self.analyse_flow()

    @property
    def max_data_stack(self):
        se = self.stack_effect
        if not se.is_known:
            return -1
        if se.no_exit:
            return 0
        else:
            return min(0, se.data_out - se.data_in)

    @property
    def MaxDataStack(self):
        return self.max_data_stack

    @property
    def max_return_stack(self):
        return 0

    @property
    def MaxReturnStack(self):
        return self.max_return_stack


class WordNative(Word):
    def __init__(self, owner, name, code, stack_effect=None):
        super().__init__(owner, name)
        self.code = code
        if stack_effect is not None:
            self.stack_effect = stack_effect

    def run(self, cpu):
        self.code(cpu)


class WordInterpreted(Word):
    def __init__(self, owner, name, num_locals, code, to_resolve):
        super().__init__(owner, name)
        self.num_locals = num_locals
        self.NumLocals = num_locals
        self.code = code
        self.Code = code
        self.to_resolve = to_resolve
        self._flow_analysis = 0
        self._max_data_stack = 0
        self._max_return_stack = 0

    def resolve(self):
        if self.to_resolve is None:
            return
        for i in range(len(self.to_resolve)):
            tt = self.to_resolve[i]
            if tt is None:
                continue
            self.code[i].resolve_target(self.tc.lookup(tt))
        self.to_resolve = None

    def run(self, cpu):
        self.resolve()
        cpu.enter(self.code, self.num_locals)

    def get_references(self):
        self.resolve()
        r = []
        for op in self.code:
            w = op.get_reference(self.tc)
            if w is not None:
                r.append(w)
        return r

    def get_data_blocks(self):
        self.resolve()
        r = []
        for op in self.code:
            cd = op.get_data_block(self.tc)
            if cd is not None:
                r.append(cd)
        return r

    def generate_code_elements(self, dst):
        self.resolve()
        n = len(self.code)
        gcode = [None] * n
        for i in range(n):
            gcode[i] = self.code[i].to_code_element()
        for i in range(n):
            self.code[i].fix_up(gcode, i)
        dst.append(CodeElementUInt(to_u32(self.num_locals)))
        for i in range(n):
            dst.append(gcode[i])

    def _merge_sa(self, sa, j, c):
        if sa[j] == INT32_MIN:
            sa[j] = c
            return True
        elif sa[j] != c:
            raise Exception("In word '%s', offset %d: stack action mismatch (%d / %d)" % (self.name, j, sa[j], c))
        else:
            return False

    def analyse_flow(self):
        if self._flow_analysis == 1:
            return
        if self._flow_analysis != 0:
            raise Exception("recursive call detected in '" + self.name + "'")
        self._flow_analysis = 2
        n = len(self.code)
        sa = [INT32_MIN] * n
        sa[0] = 0
        to_explore = [0] * n
        tX = 0
        tY = 0
        off = 0

        exitSA = INT32_MIN
        mds = 0
        mrs = 0
        maxDepth = 0

        while True:
            op = self.code[off]
            mft = op.may_fall_through
            c = sa[off]
            if isinstance(op, OpcodeCall):
                w = op.get_reference(self.tc)
                w.analyse_flow()
                se = w.stack_effect
                if not se.is_known:
                    raise Exception("call from '%s' to '%s' with unknown stack effect" % (self.name, w.name))
                if se.no_exit:
                    mft = False
                    a = 0
                else:
                    a = se.data_out - se.data_in
                mds = max(mds, c + w.max_data_stack)
                mrs = max(mrs, w.max_return_stack)
                maxDepth = min(maxDepth, c - se.data_in)
            elif isinstance(op, OpcodeRet):
                if exitSA == INT32_MIN:
                    exitSA = c
                elif exitSA != c:
                    raise Exception("'%s': exit stack action mismatch: %d / %d (offset %d)" % (self.name, exitSA, c, off))
                a = 0
            else:
                a = op.stack_action
                mds = max(mds, c + a)
            c += a
            maxDepth = min(maxDepth, c)

            j = op.jump_disp
            if j != 0:
                j += off + 1
                to_explore[tY] = j
                tY += 1
                self._merge_sa(sa, j, c)
            off += 1
            if not mft or not self._merge_sa(sa, off, c):
                if tX < tY:
                    off = to_explore[tX]
                    tX += 1
                else:
                    break

        self._max_data_stack = mds
        self._max_return_stack = 1 + self.num_locals + mrs

        if exitSA == INT32_MIN:
            computed = SType(-maxDepth, -1)
        else:
            computed = SType(-maxDepth, -maxDepth + exitSA)

        if self.stack_effect.is_known:
            if not computed.is_sub_of(self.stack_effect):
                raise Exception("word '%s': computed stack effect %s does not match declared %s" % (self.name, str(computed), str(self.stack_effect)))
        else:
            self.stack_effect = computed

        self._flow_analysis = 1

    @property
    def max_data_stack(self):
        self.analyse_flow()
        return self._max_data_stack

    @property
    def max_return_stack(self):
        self.analyse_flow()
        return self._max_return_stack


class WordData(Word):
    def __init__(self, owner, name, blob_or_base, offset):
        super().__init__(owner, name)
        if isinstance(blob_or_base, ConstData):
            self.blob = blob_or_base
            self.base_blob_name = None
        else:
            self.blob = None
            self.base_blob_name = blob_or_base
        self.offset = offset
        self._ongoing = False
        self.stack_effect = SType(0, 1)

    def resolve(self):
        if self.blob is not None:
            return
        if self._ongoing:
            raise Exception("circular reference in blobs (%s)" % self.name)
        self._ongoing = True
        wd = self.tc.lookup(self.base_blob_name)
        if not isinstance(wd, WordData):
            raise Exception("data word '%s' based on non-data word '%s'" % (self.name, self.base_blob_name))
        wd.resolve()
        self.blob = wd.blob
        self.offset += wd.offset
        self._ongoing = False

    def run(self, cpu):
        self.resolve()
        cpu.push(TValue(self.offset, TPointerBlob(self.blob)))

    def get_data_blocks(self):
        self.resolve()
        return [self.blob]

    def generate_code_elements(self, dst):
        self.resolve()
        dst.append(CodeElementUInt(0))
        dst.append(CodeElementUIntInt(1, to_i32(self.blob.address + self.offset)))
        dst.append(CodeElementUInt(0))


# --------------------------------------------------------------------------
# WordBuilder
# --------------------------------------------------------------------------

class WordBuilder:
    def __init__(self, tc, name):
        self.tc = tc
        self.name = name
        self.cf_stack = []
        self.code = []
        self.to_resolve = []
        self.locals = {}
        self.jump_to_last = True
        self.stack_effect = SType.UNKNOWN

    @property
    def StackEffect(self):
        return self.stack_effect

    @StackEffect.setter
    def StackEffect(self, v):
        self.stack_effect = v

    def build(self):
        if len(self.cf_stack) != 0:
            raise Exception("control-flow stack is not empty")
        if self.jump_to_last or self.code[-1].may_fall_through:
            self.ret()
        w = WordInterpreted(self.tc, self.name, len(self.locals), list(self.code), list(self.to_resolve))
        w.stack_effect = self.stack_effect
        return w

    def Build(self):
        return self.build()

    def _add(self, op, ref_name=None):
        self.code.append(op)
        self.to_resolve.append(ref_name)
        self.jump_to_last = False

    def cs_roll(self, depth):
        depth = int(depth)
        x = self.cf_stack[len(self.cf_stack) - 1 - depth]
        # pop at that index, push on top
        del self.cf_stack[len(self.cf_stack) - 1 - depth]
        self.cf_stack.append(x)

    def CSRoll(self, depth):
        self.cs_roll(int(depth.to_int()) if isinstance(depth, TValue) else int(depth))

    def cs_pick(self, depth):
        depth = int(depth)
        x = self.cf_stack[len(self.cf_stack) - 1 - depth]
        self._cs_push(x)

    def CSPick(self, depth):
        self.cs_pick(int(depth.to_int()) if isinstance(depth, TValue) else int(depth))

    def _cs_push(self, x):
        self.cf_stack.append(x)

    def _cs_pop(self):
        return self.cf_stack.pop()

    def cs_push_orig(self):
        self._cs_push(len(self.code))

    def cs_push_dest(self):
        self._cs_push(-len(self.code) - 1)

    def _cs_pop_orig(self):
        x = self._cs_pop()
        if x < 0:
            raise Exception("not an origin")
        return x

    def _cs_pop_dest(self):
        x = self._cs_pop()
        if x >= 0:
            raise Exception("not a destination")
        return -x - 1

    def literal(self, v):
        self._add(OpcodeConst(v))

    def Literal(self, v):
        self.literal(v)

    def call(self, target):
        if isinstance(target, TPointerXT):
            if target.target is None:
                self._add(OpcodeCall(), target.name)
            else:
                self._add(OpcodeCall(target.target))
            return
        # string target
        if target.startswith(">"):
            lname = target[1:]
            write = True
        else:
            lname = target
            write = False
        if lname in self.locals:
            lnum = self.locals[lname]
            if write:
                self._add(OpcodePutLocal(lnum))
            else:
                self._add(OpcodeGetLocal(lnum))
        else:
            self._add(OpcodeCall(), target)

    def Call(self, target):
        self.call(target)

    def call_ext_word(self, wtarget):
        self._add(OpcodeCall(wtarget))

    def CallExt(self, wtarget):
        if isinstance(wtarget, Word):
            self.call_ext_word(wtarget)
        else:
            # string target, ignore locals
            self._add(OpcodeCall(), wtarget)

    def get_local(self, name):
        if name in self.locals:
            self._add(OpcodeGetLocal(self.locals[name]))
        else:
            raise Exception("no such local: " + name)

    def GetLocal(self, name):
        self.get_local(name)

    def put_local(self, name):
        if name in self.locals:
            self._add(OpcodePutLocal(self.locals[name]))
        else:
            raise Exception("no such local: " + name)

    def PutLocal(self, name):
        self.put_local(name)

    def def_local(self, lname):
        if lname in self.locals:
            raise Exception("local already defined: %s" % lname)
        self.locals[lname] = len(self.locals)

    def DefLocal(self, lname):
        self.def_local(lname)

    def ret(self):
        self._add(OpcodeRet())

    def Ret(self):
        self.ret()

    def ahead(self):
        self.cs_push_orig()
        self._add(OpcodeJumpUncond())

    def Ahead(self):
        self.ahead()

    def ahead_if(self):
        self.cs_push_orig()
        self._add(OpcodeJumpIf())

    def AheadIf(self):
        self.ahead_if()

    def ahead_if_not(self):
        self.cs_push_orig()
        self._add(OpcodeJumpIfNot())

    def AheadIfNot(self):
        self.ahead_if_not()

    def then(self):
        x = self._cs_pop_orig()
        self.code[x].resolve_jump(len(self.code) - x - 1)
        self.jump_to_last = True

    def Then(self):
        self.then()

    def begin(self):
        self.cs_push_dest()

    def Begin(self):
        self.begin()

    def again(self):
        x = self._cs_pop_dest()
        self._add(OpcodeJumpUncond(x - len(self.code) - 1))

    def Again(self):
        self.again()

    def again_if(self):
        x = self._cs_pop_dest()
        self._add(OpcodeJumpIf(x - len(self.code) - 1))

    def AgainIf(self):
        self.again_if()

    def again_if_not(self):
        x = self._cs_pop_dest()
        self._add(OpcodeJumpIfNot(x - len(self.code) - 1))

    def AgainIfNot(self):
        self.again_if_not()


# --------------------------------------------------------------------------
# T0Comp
# --------------------------------------------------------------------------

def _hex_val(c):
    if isinstance(c, str):
        c = ord(c)
    if ord('0') <= c <= ord('9'):
        return c - ord('0')
    elif ord('A') <= c <= ord('F'):
        return c - (ord('A') - 10)
    elif ord('a') <= c <= ord('f'):
        return c - (ord('a') - 10)
    else:
        return -1


def _single_char_escape(c):
    if isinstance(c, str):
        c = ord(c)
    if c == ord('n'):
        return '\n'
    elif c == ord('r'):
        return '\r'
    elif c == ord('t'):
        return '\t'
    elif c == ord('s'):
        return ' '
    else:
        return chr(c)


def _dec_hex(s):
    acc = 0
    for ch in s:
        d = _hex_val(ch)
        if d < 0:
            return -1
        acc = (acc << 4) + d
    return acc


def _escape_c_comment(s):
    sb = []
    for ch in s:
        o = ord(ch)
        if 33 <= o <= 126 and ch != '%':
            sb.append(ch)
        elif o < 0x100:
            sb.append("%%%02X" % o)
        elif o < 0x800:
            sb.append("%%%02X%%%02X" % (((o >> 6) | 0xC0) & 0xFF, ((o & 0x3F) | 0x80) & 0xFF))
        else:
            sb.append("%%%02X%%%02X%%%02X" % (((o >> 12) | 0xE0) & 0xFF, (((o >> 6) & 0x3F) | 0x80) & 0xFF, ((o & 0x3F) | 0x80) & 0xFF))
    return "".join(sb).replace("*/", "%2A/")


C_HEADER = """/* Automatically generated code; do not modify directly. */

#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint32_t *dp;
	uint32_t *rp;
	const unsigned char *ip;
} t0_context;

static uint32_t
t0_parse7E_unsigned(const unsigned char **p)
{
	uint32_t x;

	x = 0;
	for (;;) {
		unsigned y;

		y = *(*p) ++;
		x = (x << 7) | (uint32_t)(y & 0x7F);
		if (y < 0x80) {
			return x;
		}
	}
}

static int32_t
t0_parse7E_signed(const unsigned char **p)
{
	int neg;
	uint32_t x;

	neg = ((**p) >> 6) & 1;
	x = (uint32_t)-neg;
	for (;;) {
		unsigned y;

		y = *(*p) ++;
		x = (x << 7) | (uint32_t)(y & 0x7F);
		if (y < 0x80) {
			if (neg) {
				return -(int32_t)~x - 1;
			} else {
				return (int32_t)x;
			}
		}
	}
}

#define T0_VBYTE(x, n)   (unsigned char)((((uint32_t)(x) >> (n)) & 0x7F) | 0x80)
#define T0_FBYTE(x, n)   (unsigned char)(((uint32_t)(x) >> (n)) & 0x7F)
#define T0_SBYTE(x)      (unsigned char)((((uint32_t)(x) >> 28) + 0xF8) ^ 0xF8)
#define T0_INT1(x)       T0_FBYTE(x, 0)
#define T0_INT2(x)       T0_VBYTE(x, 7), T0_FBYTE(x, 0)
#define T0_INT3(x)       T0_VBYTE(x, 14), T0_VBYTE(x, 7), T0_FBYTE(x, 0)
#define T0_INT4(x)       T0_VBYTE(x, 21), T0_VBYTE(x, 14), T0_VBYTE(x, 7), T0_FBYTE(x, 0)
#define T0_INT5(x)       T0_SBYTE(x), T0_VBYTE(x, 21), T0_VBYTE(x, 14), T0_VBYTE(x, 7), T0_FBYTE(x, 0)

/* static const unsigned char t0_datablock[]; */
"""

C_ENTER = """#define T0_ENTER(ip, rp, slot)   do { \\
		const unsigned char *t0_newip; \\
		uint32_t t0_lnum; \\
		t0_newip = &t0_codeblock[t0_caddr[(slot) - T0_INTERPRETED]]; \\
		t0_lnum = t0_parse7E_unsigned(&t0_newip); \\
		(rp) += t0_lnum; \\
		*((rp) ++) = (uint32_t)((ip) - &t0_codeblock[0]) + (t0_lnum << 16); \\
		(ip) = t0_newip; \\
	} while (0)"""

C_DEFENTRY = """#define T0_DEFENTRY(name, slot) \\
void \\
name(void *ctx) \\
{ \\
	t0_context *t0ctx = ctx; \\
	t0ctx->ip = &t0_codeblock[0]; \\
	T0_ENTER(t0ctx->ip, t0ctx->rp, slot); \\
}"""

C_RUN_HEAD = """{
	uint32_t *dp, *rp;
	const unsigned char *ip;

#define T0_LOCAL(x)    (*(rp - 2 - (x)))
#define T0_POP()       (*-- dp)
#define T0_POPi()      (*(int32_t *)(-- dp))
#define T0_PEEK(x)     (*(dp - 1 - (x)))
#define T0_PEEKi(x)    (*(int32_t *)(dp - 1 - (x)))
#define T0_PUSH(v)     do { *dp = (v); dp ++; } while (0)
#define T0_PUSHi(v)    do { *(int32_t *)dp = (v); dp ++; } while (0)
#define T0_RPOP()      (*-- rp)
#define T0_RPOPi()     (*(int32_t *)(-- rp))
#define T0_RPUSH(v)    do { *rp = (v); rp ++; } while (0)
#define T0_RPUSHi(v)   do { *(int32_t *)rp = (v); rp ++; } while (0)
#define T0_ROLL(x)     do { \\
	size_t t0len = (size_t)(x); \\
	uint32_t t0tmp = *(dp - 1 - t0len); \\
	memmove(dp - t0len - 1, dp - t0len, t0len * sizeof *dp); \\
	*(dp - 1) = t0tmp; \\
} while (0)
#define T0_SWAP()      do { \\
	uint32_t t0tmp = *(dp - 2); \\
	*(dp - 2) = *(dp - 1); \\
	*(dp - 1) = t0tmp; \\
} while (0)
#define T0_ROT()       do { \\
	uint32_t t0tmp = *(dp - 3); \\
	*(dp - 3) = *(dp - 2); \\
	*(dp - 2) = *(dp - 1); \\
	*(dp - 1) = t0tmp; \\
} while (0)
#define T0_NROT()       do { \\
	uint32_t t0tmp = *(dp - 1); \\
	*(dp - 1) = *(dp - 2); \\
	*(dp - 2) = *(dp - 3); \\
	*(dp - 3) = t0tmp; \\
} while (0)
#define T0_PICK(x)      do { \\
	uint32_t t0depth = (x); \\
	T0_PUSH(T0_PEEK(t0depth)); \\
} while (0)
#define T0_CO()         do { \\
	goto t0_exit; \\
} while (0)
#define T0_RET()        goto t0_next

	dp = ((t0_context *)t0ctx)->dp;
	rp = ((t0_context *)t0ctx)->rp;
	ip = ((t0_context *)t0ctx)->ip;
	goto t0_next;
	for (;;) {
		uint32_t t0x;

	t0_next:
		t0x = T0_NEXT(&ip);
		if (t0x < T0_INTERPRETED) {
			switch (t0x) {
				int32_t t0off;

			case 0: /* ret */
				t0x = T0_RPOP();
				rp -= (t0x >> 16);
				t0x &= 0xFFFF;
				if (t0x == 0) {
					ip = NULL;
					goto t0_exit;
				}
				ip = &t0_codeblock[t0x];
				break;
			case 1: /* literal constant */
				T0_PUSHi(t0_parse7E_signed(&ip));
				break;
			case 2: /* read local */
				T0_PUSH(T0_LOCAL(t0_parse7E_unsigned(&ip)));
				break;
			case 3: /* write local */
				T0_LOCAL(t0_parse7E_unsigned(&ip)) = T0_POP();
				break;
			case 4: /* jump */
				t0off = t0_parse7E_signed(&ip);
				ip += t0off;
				break;
			case 5: /* jump if */
				t0off = t0_parse7E_signed(&ip);
				if (T0_POP()) {
					ip += t0off;
				}
				break;
			case 6: /* jump if not */
				t0off = t0_parse7E_signed(&ip);
				if (!T0_POP()) {
					ip += t0off;
				}
				break;"""

C_RUN_TAIL = """			}

		} else {
			T0_ENTER(ip, rp, t0x);
		}
	}
t0_exit:
	((t0_context *)t0ctx)->dp = dp;
	((t0_context *)t0ctx)->rp = rp;
	((t0_context *)t0ctx)->ip = ip;
}"""


class T0Comp:
    def __init__(self):
        self.token_builder = []
        self.words = {}
        self.last_word = None
        self.word_builder = None
        self.saved_word_builders = []
        self.all_c_code = {}
        self.compiling = False
        self.quit_run_loop = False
        self.extra_code = []
        self.extra_code_defer = []
        self.data_block = None
        self.current_blob_id = 0
        self.enable_flow_analysis = True
        self.ds_limit = 32
        self.rs_limit = 32
        # input state
        self.input_text = ""
        self.input_pos = 0
        self.delayed_char = INT32_MIN
        self.delayed_token = None
        self._register_natives()

    # -- helpers -----------------------------------------------------

    def next_blob_id(self):
        v = self.current_blob_id
        self.current_blob_id += 1
        return v

    def NextBlobID(self):
        return self.next_blob_id()

    def add_native(self, name, immediate, stack_effect_or_code, code=None):
        if code is None:
            c = stack_effect_or_code
            se = SType.UNKNOWN
        else:
            se = stack_effect_or_code
            c = code
        if name in self.words:
            raise Exception("Word already defined: " + name)
        w = WordNative(self, name, c)
        w.immediate = immediate
        w.stack_effect = se
        self.words[name] = w
        return w

    def AddNative(self, name, immediate, *args):
        if len(args) == 1:
            return self.add_native(name, immediate, args[0])
        else:
            return self.add_native(name, immediate, args[0], args[1])

    def lookup(self, name):
        w = self.lookup_nf(name)
        if w is not None:
            return w
        raise Exception("No such word: '%s'" % name)

    def Lookup(self, name):
        return self.lookup(name)

    def lookup_nf(self, name):
        return self.words.get(name)

    def LookupNF(self, name):
        return self.lookup_nf(name)

    def string_to_blob(self, s):
        return TValue(0, TPointerBlob(self, s))

    def StringToBlob(self, s):
        return self.string_to_blob(s)

    # -- native registration ------------------------------------------

    def _register_natives(self):
        tc = self

        def add(name, imm, *a):
            return self.add_native(name, imm, *a)

        # add-cc:
        def _add_cc(cpu):
            tt = self.next_token()
            if tt is None:
                raise Exception("EOF reached (missing name)")
            if tt in self.all_c_code:
                raise Exception("C code already set for: " + tt)
            self.all_c_code[tt] = self.parse_c_code()
        add("add-cc:", False, SType.BLANK, _add_cc)

        # cc:
        def _cc(cpu):
            tt = self.next_token()
            if tt is None:
                raise Exception("EOF reached (missing name)")
            def _c_only(cpu2, _tt=tt):
                raise Exception("C-only word: " + _tt)
            w = self.add_native(tt, False, _c_only)
            if tt in self.all_c_code:
                raise Exception("C code already set for: " + tt)
            se = [None]
            ccode = self.parse_c_code_se(se)
            self.all_c_code[tt] = ccode
            w.stack_effect = se[0]
        add("cc:", False, SType.BLANK, _cc)

        def _preamble(cpu):
            self.extra_code.append(self.parse_c_code())
        add("preamble", False, SType.BLANK, _preamble)

        def _postamble(cpu):
            self.extra_code_defer.append(self.parse_c_code())
        add("postamble", False, SType.BLANK, _postamble)

        def _make_cx(cpu):
            c = cpu.pop()
            if not isinstance(c.ptr, TPointerBlob):
                raise Exception("'%s' is not a string" % c.to_string())
            mx = cpu.pop().to_int()
            mn = cpu.pop().to_int()
            tv = TValue(0, TPointerExpr(c.to_string(), mn, mx))
            cpu.push(tv)
        add("make-CX", False, SType(3, 1), _make_cx)

        def _CX(cpu):
            tt = self.next_token()
            if tt is None:
                raise Exception("EOF reached (missing min value)")
            mn = self.parse_integer(tt)
            tt = self.next_token()
            if tt is None:
                raise Exception("EOF reached (missing max value)")
            mx = self.parse_integer(tt)
            if mx < mn:
                raise Exception("min/max in wrong order")
            tv = TValue(0, TPointerExpr(self.parse_c_code().strip(), mn, mx))
            if self.compiling:
                self.word_builder.literal(tv)
            else:
                cpu.push(tv)
        add("CX", True, _CX)

        def _co(cpu):
            raise Exception("No coroutine in compile mode")
        add("co", False, SType.BLANK, _co)

        def _colon(cpu):
            tt = self.next_token()
            if tt is None:
                raise Exception("EOF reached (missing name)")
            if self.compiling:
                self.saved_word_builders.append(self.word_builder)
            else:
                self.compiling = True
            self.word_builder = WordBuilder(self, tt)
            tt = self.next_token()
            if tt is None:
                raise Exception("EOF reached (while compiling)")
            if tt == "(":
                se = self.parse_stack_effect_nf()
                if not se.is_known:
                    raise Exception("Invalid stack effect syntax")
                self.word_builder.stack_effect = se
            else:
                self.delayed_token = tt
        add(":", False, _colon)

        def _define_word(cpu):
            dout = cpu.pop().to_int()
            din = cpu.pop().to_int()
            s = cpu.pop()
            if not isinstance(s.ptr, TPointerBlob):
                raise Exception("Not a string: '%s'" % s.to_string())
            tt = s.to_string()
            if self.compiling:
                self.saved_word_builders.append(self.word_builder)
            else:
                self.compiling = True
            self.word_builder = WordBuilder(self, tt)
            self.word_builder.stack_effect = SType(din, dout)
        add("define-word", False, _define_word)

        def _semi(cpu):
            if not self.compiling:
                raise Exception("Not compiling")
            w = self.word_builder.build()
            name = w.name
            if name in self.words:
                raise Exception("Word already defined: " + name)
            self.words[name] = w
            self.last_word = w
            if len(self.saved_word_builders) > 0:
                self.word_builder = self.saved_word_builders.pop()
            else:
                self.word_builder = None
                self.compiling = False
        add(";", True, _semi)

        def _immediate(cpu):
            if self.last_word is None:
                raise Exception("No word defined yet")
            self.last_word.immediate = True
        add("immediate", False, _immediate)

        def _literal(cpu):
            self.check_compiling()
            self.word_builder.literal(cpu.pop())
        wliteral = add("literal", True, _literal)

        def _compile(cpu):
            self.check_compiling()
            self.word_builder.call(cpu.pop().to_xt())
        wcompile = add("compile", False, _compile)

        def _postpone(cpu):
            self.check_compiling()
            tt = self.next_token()
            if tt is None:
                raise Exception("EOF reached (missing name)")
            ok, v = self.try_parse_literal(tt)
            w = self.lookup_nf(tt)
            if ok and w is not None:
                raise Exception("Ambiguous: both defined word and literal: %s" % tt)
            if ok:
                self.word_builder.literal(v)
                self.word_builder.call_ext_word(wliteral)
            elif w is not None:
                if w.immediate:
                    self.word_builder.call_ext_word(w)
                else:
                    self.word_builder.literal(TValue(0, TPointerXT(w)))
                    self.word_builder.call_ext_word(wcompile)
            else:
                self.word_builder.literal(TValue(0, TPointerXT(tt)))
                self.word_builder.call_ext_word(wcompile)
        add("postpone", True, _postpone)

        def _exitvm(cpu):
            raise Exception()
        add("exitvm", False, _exitvm)

        def _new_data_block(cpu):
            self.data_block = ConstData(self)
            cpu.push(TValue(0, TPointerBlob(self.data_block)))
        add("new-data-block", False, _new_data_block)

        def _define_data_word(cpu):
            name = cpu.pop().to_string()
            va = cpu.pop()
            tb = va.ptr if isinstance(va.ptr, TPointerBlob) else None
            if tb is None:
                raise Exception("Address is not a data area")
            w = WordData(self, name, tb.blob, va.x)
            if name in self.words:
                raise Exception("Word already defined: " + name)
            self.words[name] = w
            self.last_word = w
        add("define-data-word", False, _define_data_word)

        def _current_data(cpu):
            if self.data_block is None:
                raise Exception("No current data block")
            cpu.push(TValue(self.data_block.Length, TPointerBlob(self.data_block)))
        add("current-data", False, _current_data)

        def _data_add8(cpu):
            if self.data_block is None:
                raise Exception("No current data block")
            v = cpu.pop().to_int()
            if v < 0 or v > 0xFF:
                raise Exception("Byte value out of range: %d" % v)
            self.data_block.add8(v)
        add("data-add8", False, _data_add8)

        def _data_set8(cpu):
            va = cpu.pop()
            tb = va.ptr if isinstance(va.ptr, TPointerBlob) else None
            if tb is None:
                raise Exception("Address is not a data area")
            v = cpu.pop().to_int()
            if v < 0 or v > 0xFF:
                raise Exception("Byte value out of range: %d" % v)
            tb.blob.set8(va.x, v)
        add("data-set8", False, _data_set8)

        def _data_get8(cpu):
            va = cpu.pop()
            tb = va.ptr if isinstance(va.ptr, TPointerBlob) else None
            if tb is None:
                raise Exception("Address is not a data area")
            v = tb.blob.read8(va.x)
            cpu.push(TValue.from_int(v))
        add("data-get8", False, SType(1, 1), _data_get8)

        def _compile_local_read(cpu):
            self.check_compiling()
            self.word_builder.get_local(cpu.pop().to_string())
        add("compile-local-read", False, _compile_local_read)

        def _compile_local_write(cpu):
            self.check_compiling()
            self.word_builder.put_local(cpu.pop().to_string())
        add("compile-local-write", False, _compile_local_write)

        def _ahead(cpu):
            self.check_compiling()
            self.word_builder.ahead()
        add("ahead", True, _ahead)

        def _begin(cpu):
            self.check_compiling()
            self.word_builder.begin()
        add("begin", True, _begin)

        def _again(cpu):
            self.check_compiling()
            self.word_builder.again()
        add("again", True, _again)

        def _until(cpu):
            self.check_compiling()
            self.word_builder.again_if_not()
        add("until", True, _until)

        def _untilnot(cpu):
            self.check_compiling()
            self.word_builder.again_if()
        add("untilnot", True, _untilnot)

        def _if(cpu):
            self.check_compiling()
            self.word_builder.ahead_if_not()
        add("if", True, _if)

        def _ifnot(cpu):
            self.check_compiling()
            self.word_builder.ahead_if()
        add("ifnot", True, _ifnot)

        def _then(cpu):
            self.check_compiling()
            self.word_builder.then()
        add("then", True, _then)

        def _cs_pick(cpu):
            self.check_compiling()
            self.word_builder.CSPick(cpu.pop())
        add("cs-pick", False, _cs_pick)

        def _cs_roll(cpu):
            self.check_compiling()
            self.word_builder.CSRoll(cpu.pop())
        add("cs-roll", False, _cs_roll)

        def _next_word(cpu):
            s = self.next_token()
            if s is None:
                raise Exception("No next word (EOF)")
            cpu.push(self.string_to_blob(s))
        add("next-word", False, _next_word)

        def _parse(cpu):
            d = cpu.pop().to_int()
            s = self.read_term(d)
            cpu.push(self.string_to_blob(s))
        add("parse", False, _parse)

        def _char(cpu):
            c = self.next_char()
            if c < 0:
                raise Exception("No next character (EOF)")
            cpu.push(TValue.from_int(c))
        add("char", False, _char)

        def _tick(cpu):
            name = self.next_token()
            cpu.push(TValue(0, TPointerXT(name)))
        add("'", False, _tick)

        def _execute(cpu):
            cpu.pop().execute(self, cpu)
        add("execute", False, _execute)

        def _lb(cpu):
            self.check_compiling()
            self.compiling = False
        add("[", True, _lb)

        def _rb(cpu):
            self.compiling = True
        add("]", False, _rb)

        def _local(cpu):
            self.check_compiling()
            self.word_builder.def_local(cpu.pop().to_string())
        add("(local)", False, _local)

        def _ret(cpu):
            self.check_compiling()
            self.word_builder.ret()
        add("ret", True, _ret)

        def _drop(cpu):
            cpu.pop()
        add("drop", False, SType(1, 0), _drop)

        def _dup(cpu):
            cpu.push(cpu.peek(0))
        add("dup", False, SType(1, 2), _dup)

        def _swap(cpu):
            cpu.rot(1)
        add("swap", False, SType(2, 2), _swap)

        def _over(cpu):
            cpu.push(cpu.peek(1))
        add("over", False, SType(2, 3), _over)

        def _rot(cpu):
            cpu.rot(2)
        add("rot", False, SType(3, 3), _rot)

        def _nrot(cpu):
            cpu.nrot(2)
        add("-rot", False, SType(3, 3), _nrot)

        def _roll(cpu):
            cpu.rot(cpu.pop().to_int())
        add("roll", False, SType(1, 0), _roll)

        def _pick(cpu):
            cpu.push(cpu.peek(cpu.pop().to_int()))
        add("pick", False, SType(1, 1), _pick)

        def _plus(cpu):
            b = cpu.pop()
            a = cpu.pop()
            if b.ptr is None:
                # a.x += (int)b ; preserve a.ptr (allows ptr+int)
                cpu.push(TValue(to_i32(a.x + b.to_int()), a.ptr))
            elif isinstance(a.ptr, TPointerBlob) and isinstance(b.ptr, TPointerBlob):
                cpu.push(self.string_to_blob(a.to_string() + b.to_string()))
            else:
                raise Exception("Cannot add '%s' to '%s'" % (b.to_string(), a.to_string()))
        add("+", False, SType(2, 1), _plus)

        def _minus(cpu):
            b = cpu.pop()
            a = cpu.pop()
            ap = a.ptr if isinstance(a.ptr, TPointerBlob) else None
            bp = b.ptr if isinstance(b.ptr, TPointerBlob) else None
            if ap is not None and bp is not None and ap.blob is bp.blob:
                cpu.push(TValue.from_int(to_i32(a.x - b.x)))
                return
            bx = b.to_int()
            cpu.push(TValue(to_i32(a.x - bx), a.ptr))
        add("-", False, SType(2, 1), _minus)

        def _neg(cpu):
            ax = cpu.pop().to_int()
            cpu.push(TValue.from_int(to_i32(-ax)))
        add("neg", False, SType(1, 1), _neg)

        def _mul(cpu):
            bx = cpu.pop().to_int()
            ax = cpu.pop().to_int()
            cpu.push(TValue.from_int(to_i32(ax * bx)))
        add("*", False, SType(2, 1), _mul)

        def _div(cpu):
            bx = cpu.pop().to_int()
            ax = cpu.pop().to_int()
            cpu.push(TValue.from_int(c_div(ax, bx)))
        add("/", False, SType(2, 1), _div)

        def _udiv(cpu):
            bx = cpu.pop().to_uint()
            ax = cpu.pop().to_uint()
            # emulate C# uint division then push as bits
            cpu.push(TValue.from_uint(ax // bx))
        add("u/", False, SType(2, 1), _udiv)

        def _mod(cpu):
            bx = cpu.pop().to_int()
            ax = cpu.pop().to_int()
            cpu.push(TValue.from_int(c_mod(ax, bx)))
        add("%", False, SType(2, 1), _mod)

        def _umod(cpu):
            bx = cpu.pop().to_uint()
            ax = cpu.pop().to_uint()
            cpu.push(TValue.from_uint(ax % bx))
        add("u%", False, SType(2, 1), _umod)

        def _lt(cpu):
            bx = cpu.pop().to_int()
            ax = cpu.pop().to_int()
            cpu.push(TValue.from_bool(ax < bx))
        add("<", False, SType(2, 1), _lt)

        def _le(cpu):
            bx = cpu.pop().to_int()
            ax = cpu.pop().to_int()
            cpu.push(TValue.from_bool(ax <= bx))
        add("<=", False, SType(2, 1), _le)

        def _gt(cpu):
            bx = cpu.pop().to_int()
            ax = cpu.pop().to_int()
            cpu.push(TValue.from_bool(ax > bx))
        add(">", False, SType(2, 1), _gt)

        def _ge(cpu):
            bx = cpu.pop().to_int()
            ax = cpu.pop().to_int()
            cpu.push(TValue.from_bool(ax >= bx))
        add(">=", False, SType(2, 1), _ge)

        def _eq(cpu):
            b = cpu.pop()
            a = cpu.pop()
            cpu.push(TValue.from_bool(a.equals(b)))
        add("=", False, SType(2, 1), _eq)

        def _ne(cpu):
            b = cpu.pop()
            a = cpu.pop()
            cpu.push(TValue.from_bool(not a.equals(b)))
        add("<>", False, SType(2, 1), _ne)

        def _ult(cpu):
            bx = cpu.pop().to_uint()
            ax = cpu.pop().to_uint()
            cpu.push(TValue.from_bool(ax < bx))
        add("u<", False, SType(2, 1), _ult)

        def _ule(cpu):
            bx = cpu.pop().to_uint()
            ax = cpu.pop().to_uint()
            cpu.push(TValue.from_bool(ax <= bx))
        add("u<=", False, SType(2, 1), _ule)

        def _ugt(cpu):
            bx = cpu.pop().to_uint()
            ax = cpu.pop().to_uint()
            cpu.push(TValue.from_bool(ax > bx))
        add("u>", False, SType(2, 1), _ugt)

        def _uge(cpu):
            bx = cpu.pop().to_uint()
            ax = cpu.pop().to_uint()
            cpu.push(TValue.from_bool(ax >= bx))
        add("u>=", False, SType(2, 1), _uge)

        def _and(cpu):
            bx = cpu.pop().to_uint()
            ax = cpu.pop().to_uint()
            cpu.push(TValue.from_uint(ax & bx))
        add("and", False, SType(2, 1), _and)

        def _or(cpu):
            bx = cpu.pop().to_uint()
            ax = cpu.pop().to_uint()
            cpu.push(TValue.from_uint(ax | bx))
        add("or", False, SType(2, 1), _or)

        def _xor(cpu):
            bx = cpu.pop().to_uint()
            ax = cpu.pop().to_uint()
            cpu.push(TValue.from_uint(ax ^ bx))
        add("xor", False, SType(2, 1), _xor)

        def _not(cpu):
            ax = cpu.pop().to_uint()
            cpu.push(TValue.from_uint((~ax) & 0xFFFFFFFF))
        add("not", False, SType(1, 1), _not)

        def _shl(cpu):
            count = cpu.pop().to_int()
            if count < 0 or count > 31:
                raise Exception("Invalid shift count")
            ax = cpu.pop().to_uint()
            cpu.push(TValue.from_uint((ax << count) & 0xFFFFFFFF))
        add("<<", False, SType(2, 1), _shl)

        def _shr(cpu):
            count = cpu.pop().to_int()
            if count < 0 or count > 31:
                raise Exception("Invalid shift count")
            ax = cpu.pop().to_int()
            cpu.push(TValue.from_int(ax >> count))
        add(">>", False, SType(2, 1), _shr)

        def _ushr(cpu):
            count = cpu.pop().to_int()
            if count < 0 or count > 31:
                raise Exception("Invalid shift count")
            ax = cpu.pop().to_uint()
            cpu.push(TValue.from_uint(ax >> count))
        add("u>>", False, SType(2, 1), _ushr)

        def _dot(cpu):
            sys.stdout.write(" %s" % cpu.pop().to_string())
        add(".", False, SType(1, 0), _dot)

        def _dots(cpu):
            n = cpu.depth
            for i in range(n - 1, -1, -1):
                sys.stdout.write(" %s" % cpu.peek(i).to_string())
        add(".s", False, SType.BLANK, _dots)

        def _putc(cpu):
            v = cpu.pop().to_int() & 0xFFFF
            sys.stdout.write(chr(v))
        add("putc", False, SType(1, 0), _putc)

        def _puts(cpu):
            sys.stdout.write("%s" % cpu.pop().to_string())
        add("puts", False, SType(1, 0), _puts)

        def _cr(cpu):
            sys.stdout.write("\n")
        add("cr", False, SType.BLANK, _cr)

        def _eqstr(cpu):
            s2 = cpu.pop().to_string()
            s1 = cpu.pop().to_string()
            cpu.push(TValue.from_bool(s1 == s2))
        add("eqstr", False, SType(2, 1), _eqstr)

    # -- input handling ---------------------------------------------

    def next_char(self):
        c = self.delayed_char
        if c >= 0:
            self.delayed_char = INT32_MIN
            return c
        elif c > INT32_MIN:
            self.delayed_char = -(c + 1)
            return 10
        else:
            if self.input_pos >= len(self.input_text):
                c = -1
            else:
                c = ord(self.input_text[self.input_pos])
                self.input_pos += 1
            if c == 13:  # '\r'
                if self.delayed_char >= 0:
                    c = self.delayed_char
                    self.delayed_char = INT32_MIN
                else:
                    if self.input_pos >= len(self.input_text):
                        c = -1
                    else:
                        c = ord(self.input_text[self.input_pos])
                        self.input_pos += 1
                if c != 10:
                    self.delayed_char = c
                    c = 10
            return c

    def NextChar(self):
        return self.next_char()

    def unread(self, c):
        if c < 0:
            return
        if self.delayed_char < 0:
            if self.delayed_char != INT32_MIN:
                raise Exception("Already two delayed characters")
            self.delayed_char = c
        elif c != 10:
            raise Exception("Cannot delay two characters")
        else:
            self.delayed_char = -(self.delayed_char + 1)

    def Unread(self, c):
        self.unread(c)

    @staticmethod
    def _is_ws(c):
        return c <= 32

    def next_token(self):
        r = self.delayed_token
        if r is not None:
            self.delayed_token = None
            return r
        tb = []
        while True:
            c = self.next_char()
            if c < 0:
                return None
            if not self._is_ws(c):
                break
        if c == 34:  # '"'
            return self.parse_string()
        while True:
            tb.append(chr(c))
            c = self.next_char()
            if c < 0 or self._is_ws(c):
                self.unread(c)
                return "".join(tb)

    def Next(self):
        return self.next_token()

    def parse_c_code(self):
        holder = [None]
        r = self.parse_c_code_se(holder)
        if holder[0].is_known:
            raise Exception("Stack effect forbidden in this declaration")
        return r

    def ParseCCode(self, holder=None):
        if holder is None:
            return self.parse_c_code()
        return self.parse_c_code_se(holder)

    def parse_c_code_se(self, holder):
        # holder is a 1-list to emulate out-param
        s, se = self.parse_c_code_nf()
        if s is None:
            raise Exception("Error while parsing C code")
        holder[0] = se
        return s

    def parse_c_code_nf(self):
        stack_effect = SType.UNKNOWN
        while True:
            c = self.next_char()
            if c < 0:
                return None, stack_effect
            if not self._is_ws(c):
                if c == 40:  # '('
                    if stack_effect.is_known:
                        self.unread(c)
                        return None, stack_effect
                    stack_effect = self.parse_stack_effect_nf()
                    if not stack_effect.is_known:
                        return None, stack_effect
                    continue
                elif c != 123:  # '{'
                    self.unread(c)
                    return None, stack_effect
                break
        sb = []
        count = 1
        while True:
            c = self.next_char()
            if c < 0:
                return None, stack_effect
            if c == 123:
                count += 1
            elif c == 125:
                count -= 1
                if count == 0:
                    return "".join(sb), stack_effect
            sb.append(chr(c))

    def parse_stack_effect_nf(self):
        seen_sep = False
        seen_bang = False
        din = 0
        dout = 0
        while True:
            t = self.next_token()
            if t is None:
                return SType.UNKNOWN
            if t == "--":
                if seen_sep:
                    return SType.UNKNOWN
                seen_sep = True
            elif t == ")":
                if seen_sep:
                    if seen_bang and dout == 1:
                        dout = -1
                    return SType(din, dout)
                else:
                    return SType.UNKNOWN
            else:
                if seen_sep:
                    if dout == 0 and t == "!":
                        seen_bang = True
                    dout += 1
                else:
                    din += 1

    def parse_string(self):
        sb = ['"']
        lcwb = False
        hex_num = 0
        acc = 0
        while True:
            c = self.next_char()
            if c < 0:
                raise Exception("Unfinished literal string")
            if hex_num > 0:
                d = _hex_val(c)
                if d < 0:
                    raise Exception("not an hex digit: U+%04X" % c)
                acc = (acc << 4) + d
                hex_num -= 1
                if hex_num == 0:
                    sb.append(chr(acc))
                    acc = 0
            elif lcwb:
                lcwb = False
                if c == 10:
                    self.skip_nl()
                elif c == ord('x'):
                    hex_num = 2
                elif c == ord('u'):
                    hex_num = 4
                else:
                    sb.append(_single_char_escape(c))
            else:
                if c == 34:
                    return "".join(sb)
                elif c == 92:  # backslash
                    lcwb = True
                else:
                    sb.append(chr(c))

    def skip_nl(self):
        while True:
            c = self.next_char()
            if c < 0:
                raise Exception("EOF in literal string")
            if c == 10:
                raise Exception("Unescaped newline in literal string")
            if self._is_ws(c):
                continue
            if c == 34:
                return
            raise Exception("Invalid newline escape in literal string")

    @staticmethod
    def decode_char_const(t):
        if len(t) == 1 and t[0] != '\\':
            return ord(t[0])
        if len(t) >= 2 and t[0] == '\\':
            if t[1] == 'x':
                if len(t) == 4:
                    x = _dec_hex(t[2:])
                    if x >= 0:
                        return x
            elif t[1] == 'u':
                if len(t) == 6:
                    x = _dec_hex(t[2:])
                    if x >= 0:
                        return x
            else:
                if len(t) == 2:
                    return ord(_single_char_escape(ord(t[1])))
        raise Exception("Invalid literal char: `" + t)

    def read_term(self, ct):
        sb = []
        while True:
            c = self.next_char()
            if c < 0:
                raise Exception("EOF reached before U+%04X" % (ct & 0xFFFF))
            if c == ct:
                return "".join(sb)
            sb.append(chr(c))

    def process_input(self, text):
        self.input_text = text
        self.input_pos = 0
        self.delayed_char = INT32_MIN
        # toplevel word
        tc = self

        def _toplevel(cpu):
            self.compile_step(cpu)

        w = WordNative(self, "toplevel", _toplevel)
        cpu = CPU()
        code = [OpcodeCall(w), OpcodeJumpUncond(-2)]
        self.quit_run_loop = False
        cpu.enter(code, 0)
        while True:
            if self.quit_run_loop:
                break
            op = cpu.ip_buf[cpu.ip_off]
            cpu.ip_off += 1
            op.run(cpu)

    def ProcessInput(self, text):
        self.process_input(text)

    def compile_step(self, cpu):
        tt = self.next_token()
        if tt is None:
            if self.compiling:
                raise Exception("EOF while compiling")
            self.quit_run_loop = True
            return
        ok, v = self.try_parse_literal(tt)
        w = self.lookup_nf(tt)
        if ok and w is not None:
            raise Exception("Ambiguous: both defined word and literal: %s" % tt)
        if self.compiling:
            if ok:
                self.word_builder.literal(v)
            elif w is not None:
                if w.immediate:
                    w.run(cpu)
                else:
                    self.word_builder.call_ext_word(w)
            else:
                self.word_builder.call(tt)
        else:
            if ok:
                cpu.push(v)
            elif w is not None:
                w.run(cpu)
            else:
                raise Exception("Unknown word: '%s'" % tt)

    def get_c_code(self, name):
        return self.all_c_code.get(name)

    def check_compiling(self):
        if not self.compiling:
            raise Exception("Not in compilation mode")

    def try_parse_literal(self, tt):
        if tt.startswith('"'):
            return True, self.string_to_blob(tt[1:])
        if tt.startswith("`"):
            return True, TValue.from_int(self.decode_char_const(tt[1:]))
        neg = False
        s = tt
        if s.startswith("-"):
            neg = True
            s = s[1:]
        elif s.startswith("+"):
            s = s[1:]
        radix = 10
        if s.startswith("0x") or s.startswith("0X"):
            radix = 16
            s = s[2:]
        elif s.startswith("0b") or s.startswith("0B"):
            radix = 2
            s = s[2:]
        if len(s) == 0:
            return False, TValue(0)
        acc = 0
        overflow = False
        maxV = UINT32_MAX // radix
        for ch in s:
            d = _hex_val(ch)
            if d < 0 or d >= radix:
                return False, TValue(0)
            if acc > maxV:
                overflow = True
            acc = (acc * radix) & 0xFFFFFFFF
            if d > UINT32_MAX - acc:
                overflow = True
            acc = (acc + d) & 0xFFFFFFFF
        x = to_i32(acc)
        if neg:
            if acc > 0x80000000:
                overflow = True
            x = to_i32(-x)
        if overflow:
            raise Exception("invalid literal integer (overflow)")
        return True, TValue.from_int(x)

    def TryParseLiteral(self, tt):
        return self.try_parse_literal(tt)

    def parse_integer(self, tt):
        ok, tv = self.try_parse_literal(tt)
        if not ok:
            raise Exception("not an integer: " + tt)
        return tv.to_int()

    # -- code generation ---------------------------------------------

    def generate(self, out_base, core_run, entry_points):
        # Gather words transitively
        word_set = {}
        tx = []
        for ep in entry_points:
            if ep in word_set:
                continue
            w = self.lookup(ep)
            word_set[w.name] = w
            tx.append(w)
        while len(tx) > 0:
            w = tx.pop(0)
            for w2 in w.get_references():
                if w2.name in word_set:
                    continue
                word_set[w2.name] = w2
                tx.append(w2)

        if self.enable_flow_analysis:
            for ep in entry_points:
                w = word_set[ep]
                w.analyse_flow()
                sys.stdout.write("%s: ds=%d rs=%d\n" % (ep, w.max_data_stack, w.max_return_stack))
                if w.max_data_stack > self.ds_limit:
                    raise Exception("'" + ep + "' exceeds data stack limit")
                if w.max_return_stack > self.rs_limit:
                    raise Exception("'" + ep + "' exceeds return stack limit")

        blocks = {}
        for w in word_set.values():
            for cd in w.get_data_blocks():
                blocks[cd.id] = cd
        data_len = 1
        for cd_id in sorted(blocks.keys()):
            cd = blocks[cd_id]
            cd.address = data_len
            data_len += cd.Length

        slots = {}
        cur_slot = 7
        ccode_uni = {}
        ccode_names = {}
        for wname in sorted(word_set.keys()):
            w = word_set[wname]
            ccode = self.get_c_code(w.name)
            if ccode is None:
                if isinstance(w, WordNative):
                    raise Exception("No C code for native '%s'" % w.name)
                continue
            if ccode in ccode_uni:
                sn = ccode_uni[ccode]
                ccode_names[sn] += " " + _escape_c_comment(w.name)
            else:
                sn = cur_slot
                cur_slot += 1
                ccode_uni[ccode] = sn
                ccode_names[sn] = _escape_c_comment(w.name)
            slots[w.name] = sn
            w.slot = sn

        slot_interpreted = cur_slot
        for wname in sorted(word_set.keys()):
            w = word_set[wname]
            if self.get_c_code(w.name) is not None:
                continue
            sn = cur_slot
            cur_slot += 1
            slots[w.name] = sn
            w.slot = sn
        num_interpreted = cur_slot - slot_interpreted

        for ep in entry_points:
            if self.get_c_code(ep) is not None:
                raise Exception("Non-interpreted entry point")

        gcode_list = []
        interpreted_entry = [None] * num_interpreted
        for wname in sorted(word_set.keys()):
            w = word_set[wname]
            if self.get_c_code(w.name) is not None:
                continue
            n = len(gcode_list)
            w.generate_code_elements(gcode_list)
            interpreted_entry[w.slot - slot_interpreted] = gcode_list[n]
        gcode = list(gcode_list)

        if slot_interpreted + num_interpreted >= 256:
            sys.stdout.write("WARNING: more than 255 words\n")
            one_byte_code = False
        else:
            one_byte_code = True

        total_len = -1
        gcode_len = [0] * len(gcode)
        while True:
            for i in range(len(gcode)):
                gcode_len[i] = gcode[i].get_length(one_byte_code)
            off = 0
            for i in range(len(gcode)):
                gcode[i].address = off
                gcode[i].last_length = gcode_len[i]
                off += gcode_len[i]
            if off == total_len:
                break
            total_len = off

        with open(out_base + ".c", "w", encoding="utf-8", newline="\n") as tw:
            tw.write("%s\n" % C_HEADER)
            tw.write("\n")
            for ep in entry_points:
                tw.write("void %s_init_%s(void *t0ctx);\n" % (core_run, ep))
            tw.write("\n")
            tw.write("void %s_run(void *t0ctx);\n" % core_run)
            for pp in self.extra_code:
                tw.write("\n")
                tw.write("%s\n" % pp)
            tw.write("\n")
            tw.write("static const unsigned char t0_datablock[] = {")
            bw = BlobWriter(tw, 78, 1)
            bw.append_byte(0)
            for cd_id in sorted(blocks.keys()):
                blocks[cd_id].encode(bw)
            tw.write("\n")
            tw.write("};\n")
            tw.write("\n")
            tw.write("static const unsigned char t0_codeblock[] = {")
            bw = BlobWriter(tw, 78, 1)
            for ce in gcode:
                ce.encode(bw, one_byte_code)
            tw.write("\n")
            tw.write("};\n")
            tw.write("\n")
            tw.write("static const uint16_t t0_caddr[] = {")
            for i in range(len(interpreted_entry)):
                if i != 0:
                    tw.write(",")
                tw.write("\n")
                tw.write("\t%d" % interpreted_entry[i].address)
            tw.write("\n")
            tw.write("};\n")
            tw.write("\n")
            tw.write("#define T0_INTERPRETED   %d\n" % slot_interpreted)
            tw.write("\n")
            tw.write("%s\n" % C_ENTER)
            tw.write("\n")
            tw.write("%s\n" % C_DEFENTRY)
            tw.write("\n")
            for ep in entry_points:
                tw.write("T0_DEFENTRY(%s, %d)\n" % (core_run + "_init_" + ep, word_set[ep].slot))
            tw.write("\n")
            if one_byte_code:
                tw.write("#define T0_NEXT(t0ipp)   (*(*(t0ipp)) ++)\n")
            else:
                tw.write("#define T0_NEXT(t0ipp)   t0_parse7E_unsigned(t0ipp)\n")
            tw.write("\n")
            tw.write("void\n")
            tw.write("%s_run(void *t0ctx)\n" % core_run)
            tw.write("%s\n" % C_RUN_HEAD)
            # C code cases, sorted by slot
            nccode = {}
            for k, v in ccode_uni.items():
                nccode[v] = k
            for sn in sorted(nccode.keys()):
                tw.write("\t\t\tcase %d: {\n" % sn)
                tw.write("\t\t\t\t/* %s */\n" % ccode_names[sn])
                tw.write("%s\n" % nccode[sn])
                tw.write("\t\t\t\t}\n")
                tw.write("\t\t\t\tbreak;\n")
            tw.write("%s\n" % C_RUN_TAIL)
            for pp in self.extra_code_defer:
                tw.write("\n")
                tw.write("%s\n" % pp)

        code_len = 0
        for ce in gcode:
            code_len += ce.get_length(one_byte_code)
        data_block_len = 0
        for cd in blocks.values():
            data_block_len += cd.Length
        sys.stdout.write("code length: %6d byte(s)\n" % code_len)
        sys.stdout.write("data length: %6d byte(s)\n" % data_len)
        sys.stdout.write("total words: %d (interpreted: %d)\n" % (slot_interpreted + num_interpreted, num_interpreted))

    def Generate(self, out_base, core_run, entry_points):
        self.generate(out_base, core_run, entry_points)


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def usage():
    sys.stdout.write("usage: T0Comp.exe [ options... ] file...\n")
    sys.stdout.write("options:\n")
    sys.stdout.write("   -o file    use 'file' as base for output file name (default: 't0out')\n")
    sys.stdout.write("   -r name    use 'name' as base for run function (default: same as output)\n")
    sys.stdout.write("   -m name[,name...]\n")
    sys.stdout.write("              define entry point(s)\n")
    sys.stdout.write("   -nf        disable flow analysis\n")
    sys.exit(1)


def find_kernel():
    # kern.t0 next to this script, else alongside cwd
    here = os.path.dirname(os.path.abspath(__file__))
    for cand in (os.path.join(here, "kern.t0"), os.path.join(os.getcwd(), "kern.t0")):
        if os.path.isfile(cand):
            return cand
    return None


def main(argv=None):
    if argv is None:
        argv = sys.argv[1:]
    try:
        r = []
        out_base = None
        entry_points = []
        core_run = None
        flow = True
        ds_lim = 32
        rs_lim = 32
        i = 0
        while i < len(argv):
            a = argv[i]
            if not a.startswith("-"):
                r.append(a)
                i += 1
                continue
            if a == "--":
                i += 1
                while i < len(argv):
                    r.append(argv[i])
                    i += 1
                break
            while a.startswith("-"):
                a = a[1:]
            j = a.find("=")
            if j < 0:
                pname = a.lower()
                pval = None
                pval2 = argv[i + 1] if (i + 1) < len(argv) else None
            else:
                pname = a[:j].strip().lower()
                pval = a[j + 1:]
                pval2 = None
            if pname in ("o", "out"):
                if pval is None:
                    if pval2 is None:
                        usage()
                    i += 1
                    pval = pval2
                if out_base is not None:
                    usage()
                out_base = pval
            elif pname in ("r", "run"):
                if pval is None:
                    if pval2 is None:
                        usage()
                    i += 1
                    pval = pval2
                core_run = pval
            elif pname in ("m", "main"):
                if pval is None:
                    if pval2 is None:
                        usage()
                    i += 1
                    pval = pval2
                for ep in pval.split(","):
                    epz = ep.strip()
                    if len(epz) > 0:
                        entry_points.append(epz)
            elif pname in ("nf", "noflow"):
                flow = False
            else:
                usage()
            i += 1
        if len(r) == 0:
            usage()
        if out_base is None:
            out_base = "t0out"
        if len(entry_points) == 0:
            entry_points.append("main")
        if core_run is None:
            core_run = out_base
        tc = T0Comp()
        tc.enable_flow_analysis = flow
        tc.ds_limit = ds_lim
        tc.rs_limit = rs_lim
        kp = find_kernel()
        if kp is None:
            sys.stderr.write("kern.t0 not found (looked next to t0comp.py and in cwd)\n")
            sys.exit(1)
        with open(kp, "r", encoding="utf-8") as f:
            tc.process_input(f.read())
        for a in r:
            sys.stdout.write("[%s]\n" % a)
            with open(a, "r", encoding="utf-8") as f:
                tc.process_input(f.read())
        tc.generate(out_base, core_run, entry_points)
    except SystemExit:
        raise
    except Exception as e:
        import traceback
        sys.stdout.write(str(e) + "\n")
        traceback.print_exc(file=sys.stdout)
        sys.exit(1)


if __name__ == "__main__":
    main()

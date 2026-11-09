// Legacy raw-load helper fixture generated from campaign baseline chain 52c623609245.
#pragma clang diagnostic ignored "-Wmissing-prototypes"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct _10
{
    uint _m0;
    uint _m1[8];
    float4 _m2;
    float4 _m3;
    int _m4;
    float _m5[73];
    int4 _m6;
    int _m7;
};

struct _16
{
    float4 _m0[4096];
};

struct _21
{
    uint _m0[1];
};

struct spvDescriptorSetBuffer0
{
    device _21* cp_s0 [[id(0)]];
    device _21* cp_s1 [[id(1)]];
};

static inline __attribute__((always_inline))
int _32(thread const int& _63, thread const int& _64, device _21& cp_s1)
{
    int _67 = _64 << 11;
    uint _70 = uint(int(uint(_63) >> uint(21)));
    int _72 = _67 | int(_70);
    int _74 = _72 & 4095;
    int _76 = _74 * 8;
    int _78 = _76 + 7;
    float _80 = as_type<float>(cp_s1._m0[_78]);
    bool _84 = as_type<int>(_80) != 0;
    int _86 = _76 + 6;
    float _88 = as_type<float>(cp_s1._m0[_86]);
    bool _90 = as_type<int>(_88) == _72;
    bool _92 = _84 && _90;
    bool _94 = uint(_64) < 2097152u;
    bool _96 = _92 && _94;
    int _98 = as_type<int>(_80);
    if (!_96)
    {
        int _100 = 0;
        int _102 = 0 + 4;
        float _104 = as_type<float>(cp_s1._m0[_102]);
        uint _106 = uint(int(uint(as_type<int>(_104)) >> uint(1)));
        int _108 = int(_106);
        bool _110;
        do
        {
            int _112 = _100 + _108;
            int _114 = _112 * 8;
            int _116 = _114 + 0;
            float _118 = as_type<float>(cp_s1._m0[_116]);
            int _120 = _114 + 1;
            float _122 = as_type<float>(cp_s1._m0[_120]);
            bool _124 = as_type<uint>(_122) < uint(_64);
            bool _126 = as_type<int>(_122) == _64;
            bool _128 = as_type<uint>(_118) <= uint(_63);
            bool _130 = _126 && _128;
            bool _132 = _124 || _130;
            int _134 = _132 ? _112 : _100;
            _100 = _134;
            uint _136 = uint(int(uint(_108) >> uint(1)));
            _108 = int(_136);
            _110 = _108 != 0;
        } while (_110);
        _98 = _100;
    }
    int _138 = _98 * 8;
    int _140 = _138 + 2;
    float _142 = as_type<float>(cp_s1._m0[_140]);
    int _144 = _138 + 3;
    float _146 = as_type<float>(cp_s1._m0[_144]);
    bool _148 = uint(_64) < as_type<uint>(_146);
    bool _150 = _64 == as_type<int>(_146);
    bool _152 = uint(_63) < as_type<uint>(_142);
    bool _154 = _150 && _152;
    bool _156 = _148 || _154;
    if (_156)
    {
        int _158 = _138 + 4;
        float _160 = as_type<float>(cp_s1._m0[_158]);
        int _162 = _63 + as_type<int>(_160);
        bool _164 = uint(_162) < uint(_63);
        int _166 = int(_164);
        int _168 = _138 + 5;
        float _170 = as_type<float>(cp_s1._m0[_168]);
        int _172 = _64 + as_type<int>(_170);
        int _174 = _172 + _166;
        uint _376 = *(reinterpret_cast<device uint*>(as_type<ulong>(uint2(uint(_162), uint(_174)))));
        float _176 = as_type<float>(_376);
        return as_type<int>(_176);
    }
    return 0;
}

kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])
{
    float _36 = as_type<float>((*spvDescriptorSet0.cp_s0)._m0[0]);
    float _38 = as_type<float>((*spvDescriptorSet0.cp_s0)._m0[1]);
    int _40 = as_type<int>(_36);
    int _42 = as_type<int>(_38);
    int _44 = _32(_40, _42, (*spvDescriptorSet0.cp_s1));
    (*spvDescriptorSet0.cp_s0)._m0[2] = uint(_44);
}

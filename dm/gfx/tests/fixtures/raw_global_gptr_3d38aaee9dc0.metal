// Generated from synthetic instructions by test_dmsh_ampere_mem.RawGlobal
// test_the_search_takes_the_tables_count with the pinned GPTR translator.
// No game-derived input. Only the translated scalar helper is retained.
static inline __attribute__((always_inline))
int _38(thread const int& _71, thread const int& _72, thread const int& _73, device _21& cp_s1, device _21& cp_s2, thread uint& _32)
{
    bool _132 = false;
    bool _78 = _73 != (-1);
    int _124;
    int _130;
    if (_78)
    {
        int _80 = _73 * 8;
        int _82 = _80 + 1;
        float _84 = as_type<float>(cp_s2._m0[_82]);
        int _86 = _80 + 0;
        float _88 = as_type<float>(cp_s2._m0[_86]);
        bool _90 = as_type<uint>(_84) < uint(_72);
        bool _92 = as_type<int>(_84) == _72;
        bool _94 = as_type<uint>(_88) <= uint(_71);
        bool _96 = _92 && _94;
        bool _98 = _90 || _96;
        int _100 = _80 + 3;
        float _102 = as_type<float>(cp_s2._m0[_100]);
        int _104 = _80 + 2;
        float _106 = as_type<float>(cp_s2._m0[_104]);
        bool _108 = uint(_72) < as_type<uint>(_102);
        bool _110 = _72 == as_type<int>(_102);
        bool _112 = uint(_71) < as_type<uint>(_106);
        bool _114 = _110 && _112;
        bool _116 = _108 || _114;
        bool _118 = _98 && _116;
        if (_118)
        {
            int _120 = _80 + 4;
            float _122 = as_type<float>(cp_s2._m0[_120]);
            _124 = as_type<int>(_122);
            int _126 = _80 + 5;
            float _128 = as_type<float>(cp_s2._m0[_126]);
            _130 = as_type<int>(_128);
            _132 = true;
        }
    }
    int _245;
    bool _263;
    if (!_132)
    {
        float _134 = as_type<float>(_32);
        int _136 = as_type<int>(_134) * 8;
        int _138 = _136 + 1;
        float _140 = as_type<float>(cp_s1._m0[_138]);
        int _142 = _136 + 0;
        float _144 = as_type<float>(cp_s1._m0[_142]);
        bool _146 = as_type<uint>(_140) < uint(_72);
        bool _148 = as_type<int>(_140) == _72;
        bool _150 = as_type<uint>(_144) <= uint(_71);
        bool _152 = _148 && _150;
        bool _154 = _146 || _152;
        int _156 = _136 + 3;
        float _158 = as_type<float>(cp_s1._m0[_156]);
        int _160 = _136 + 2;
        float _162 = as_type<float>(cp_s1._m0[_160]);
        bool _164 = uint(_72) < as_type<uint>(_158);
        bool _166 = _72 == as_type<int>(_158);
        bool _168 = uint(_71) < as_type<uint>(_162);
        bool _170 = _166 && _168;
        bool _172 = _164 || _170;
        bool _174 = _154 && _172;
        int _176 = as_type<int>(_134);
        if (!_174)
        {
            int _178 = _72 << 11;
            uint _181 = uint(int(uint(_71) >> uint(21)));
            int _183 = _178 | int(_181);
            int _185 = _183 & 4095;
            int _187 = _185 * 8;
            int _189 = _187 + 7;
            float _191 = as_type<float>(cp_s1._m0[_189]);
            bool _193 = as_type<int>(_191) != 0;
            int _195 = _187 + 6;
            float _197 = as_type<float>(cp_s1._m0[_195]);
            bool _199 = as_type<int>(_197) == _183;
            bool _201 = _193 && _199;
            bool _203 = uint(_72) < 2097152u;
            bool _205 = _201 && _203;
            _176 = as_type<int>(_191);
            if (!_205)
            {
                int _207 = 0;
                int _209 = 0 + 4;
                float _211 = as_type<float>(cp_s1._m0[_209]);
                uint _213 = uint(int(uint(as_type<int>(_211)) >> uint(1)));
                int _215 = int(_213);
                bool _217;
                do
                {
                    int _219 = _207 + _215;
                    int _221 = _219 * 8;
                    int _223 = _221 + 0;
                    float _225 = as_type<float>(cp_s1._m0[_223]);
                    int _227 = _221 + 1;
                    float _229 = as_type<float>(cp_s1._m0[_227]);
                    bool _231 = as_type<uint>(_229) < uint(_72);
                    bool _233 = as_type<int>(_229) == _72;
                    bool _235 = as_type<uint>(_225) <= uint(_71);
                    bool _237 = _233 && _235;
                    bool _239 = _231 || _237;
                    int _241 = _239 ? _219 : _207;
                    _207 = _241;
                    uint _243 = uint(int(uint(_215) >> uint(1)));
                    _215 = int(_243);
                    _217 = _215 != 0;
                } while (_217);
                _176 = _207;
            }
            _32 = uint(_176);
        }
        _245 = _176 * 8;
        int _247 = _245 + 2;
        float _249 = as_type<float>(cp_s1._m0[_247]);
        int _251 = _245 + 3;
        float _253 = as_type<float>(cp_s1._m0[_251]);
        bool _255 = uint(_72) < as_type<uint>(_253);
        bool _257 = _72 == as_type<int>(_253);
        bool _259 = uint(_71) < as_type<uint>(_249);
        bool _261 = _257 && _259;
        _263 = _255 || _261;
    }
    if (_263 || _132)
    {
        if (!_132)
        {
            int _265 = _245 + 4;
            float _267 = as_type<float>(cp_s1._m0[_265]);
            _124 = as_type<int>(_267);
            int _269 = _245 + 5;
            float _271 = as_type<float>(cp_s1._m0[_269]);
            _130 = as_type<int>(_271);
        }
        _132 = false;
        int _273 = _71 + _124;
        bool _275 = uint(_273) < uint(_71);
        int _277 = int(_275);
        int _279 = _72 + _130;
        int _281 = _279 + _277;
        uint _671 = *(reinterpret_cast<device uint*>(as_type<ulong>(uint2(uint(_273), uint(_281)))));
        float _283 = as_type<float>(_671);
        return as_type<int>(_283);
    }
    return 0;
}

#pragma once
// InstructionSet.cpp
// Compile by using: cl /EHsc /W4 InstructionSet.cpp
// processor: x86, x64
// Uses the __cpuid intrinsic to get information about
// CPU extended instruction set support.

#include <array>
#include <bitset>
#include <cstring>
#include <string>
#include <intrin.h>

class InstructionSet
{
    // forward declarations
    class InstructionSet_Internal;

public:
    // getters
    static std::string Vendor( void ) { return CPU_Rep.vendor_; }
    static std::string Brand( void ) { return CPU_Rep.brand_; }

    static bool SSE3( void ) { return CPU_Rep.f_1_ECX_[0]; }
    static bool PCLMULQDQ( void ) { return CPU_Rep.f_1_ECX_[1]; }
    static bool MONITOR( void ) { return CPU_Rep.f_1_ECX_[3]; }
    static bool SSSE3( void ) { return CPU_Rep.f_1_ECX_[9]; }
    static bool FMA( void ) { return AVX() && CPU_Rep.f_1_ECX_[12]; }
    static bool CMPXCHG16B( void ) { return CPU_Rep.f_1_ECX_[13]; }
    static bool SSE41( void ) { return CPU_Rep.f_1_ECX_[19]; }
    static bool SSE42( void ) { return CPU_Rep.f_1_ECX_[20]; }
    static bool MOVBE( void ) { return CPU_Rep.f_1_ECX_[22]; }
    static bool POPCNT( void ) { return CPU_Rep.f_1_ECX_[23]; }
    static bool AES( void ) { return CPU_Rep.f_1_ECX_[25]; }
    static bool XSAVE( void ) { return CPU_Rep.f_1_ECX_[26]; }
    static bool OSXSAVE( void ) { return CPU_Rep.f_1_ECX_[27]; }
    static bool AVX( void ) { return CPU_Rep.f_1_ECX_[28] && OSAVXStateEnabled(); }
    static bool F16C( void ) { return AVX() && CPU_Rep.f_1_ECX_[29]; }
    static bool RDRAND( void ) { return CPU_Rep.f_1_ECX_[30]; }

    static bool MSR( void ) { return CPU_Rep.f_1_EDX_[5]; }
    static bool CX8( void ) { return CPU_Rep.f_1_EDX_[8]; }
    static bool SEP( void ) { return CPU_Rep.f_1_EDX_[11]; }
    static bool CMOV( void ) { return CPU_Rep.f_1_EDX_[15]; }
    static bool CLFSH( void ) { return CPU_Rep.f_1_EDX_[19]; }
    static bool MMX( void ) { return CPU_Rep.f_1_EDX_[23]; }
    static bool FXSR( void ) { return CPU_Rep.f_1_EDX_[24]; }
    static bool SSE( void ) { return CPU_Rep.f_1_EDX_[25]; }
    static bool SSE2( void ) { return CPU_Rep.f_1_EDX_[26]; }

    static bool FSGSBASE( void ) { return CPU_Rep.f_7_EBX_[0]; }
    static bool BMI1( void ) { return CPU_Rep.f_7_EBX_[3]; }
    static bool HLE( void ) { return CPU_Rep.isIntel_ && CPU_Rep.f_7_EBX_[4]; }
    static bool AVX2( void ) { return AVX() && CPU_Rep.f_7_EBX_[5]; }
    static bool BMI2( void ) { return CPU_Rep.f_7_EBX_[8]; }
    static bool ERMS( void ) { return CPU_Rep.f_7_EBX_[9]; }
    static bool INVPCID( void ) { return CPU_Rep.f_7_EBX_[10]; }
    static bool RTM( void ) { return CPU_Rep.isIntel_ && CPU_Rep.f_7_EBX_[11]; }
    static bool AVX512F( void ) { return CPU_Rep.f_7_EBX_[16]; }
    static bool RDSEED( void ) { return CPU_Rep.f_7_EBX_[18]; }
    static bool ADX( void ) { return CPU_Rep.f_7_EBX_[19]; }
    static bool AVX512PF( void ) { return CPU_Rep.f_7_EBX_[26]; }
    static bool AVX512ER( void ) { return CPU_Rep.f_7_EBX_[27]; }
    static bool AVX512CD( void ) { return CPU_Rep.f_7_EBX_[28]; }
    static bool SHA( void ) { return CPU_Rep.f_7_EBX_[29]; }

    static bool PREFETCHWT1( void ) { return CPU_Rep.f_7_ECX_[0]; }

    static bool LAHF( void ) { return CPU_Rep.f_81_ECX_[0]; }
    static bool LZCNT( void ) { return CPU_Rep.isIntel_ && CPU_Rep.f_81_ECX_[5]; }
    static bool ABM( void ) { return CPU_Rep.isAMD_ && CPU_Rep.f_81_ECX_[5]; }
    static bool SSE4a( void ) { return CPU_Rep.isAMD_ && CPU_Rep.f_81_ECX_[6]; }
    static bool XOP( void ) { return CPU_Rep.isAMD_ && CPU_Rep.f_81_ECX_[11]; }
    static bool TBM( void ) { return CPU_Rep.isAMD_ && CPU_Rep.f_81_ECX_[21]; }

    static bool SYSCALL( void ) { return CPU_Rep.isIntel_ && CPU_Rep.f_81_EDX_[11]; }
    static bool MMXEXT( void ) { return CPU_Rep.isAMD_ && CPU_Rep.f_81_EDX_[22]; }
    static bool RDTSCP( void ) { return CPU_Rep.isIntel_ && CPU_Rep.f_81_EDX_[27]; }
    static bool _3DNOWEXT( void ) { return CPU_Rep.isAMD_ && CPU_Rep.f_81_EDX_[30]; }
    static bool _3DNOW( void ) { return CPU_Rep.isAMD_ && CPU_Rep.f_81_EDX_[31]; }

private:
    static bool OSAVXStateEnabled() {
        if ( !CPU_Rep.f_1_ECX_[27] ) {
            return false;
        }
        return (_xgetbv( 0 ) & 0x6) == 0x6;
    }

    static const InstructionSet_Internal CPU_Rep;

    class InstructionSet_Internal
    {
    public:
        InstructionSet_Internal()
            : nIds_{ 0 },
            nExIds_{ 0 },
            isIntel_{ false },
            isAMD_{ false },
            f_1_ECX_{ 0 },
            f_1_EDX_{ 0 },
            f_7_EBX_{ 0 },
            f_7_ECX_{ 0 },
            f_81_ECX_{ 0 },
            f_81_EDX_{ 0 }
        {
            std::array<int, 4> cpuInfo{};
            __cpuidex( cpuInfo.data(), 0, 0 );
            nIds_ = static_cast<unsigned int>(cpuInfo[0]);

            std::array<char, 13> vendor{};
            memcpy( vendor.data(), &cpuInfo[1], sizeof( int ) );
            memcpy( vendor.data() + 4, &cpuInfo[3], sizeof( int ) );
            memcpy( vendor.data() + 8, &cpuInfo[2], sizeof( int ) );
            vendor_ = vendor.data();
            isIntel_ = vendor_ == "GenuineIntel";
            isAMD_ = vendor_ == "AuthenticAMD";

            if ( nIds_ >= 1 ) {
                __cpuidex( cpuInfo.data(), 1, 0 );
                f_1_ECX_ = static_cast<unsigned int>(cpuInfo[2]);
                f_1_EDX_ = static_cast<unsigned int>(cpuInfo[3]);
            }
            if ( nIds_ >= 7 ) {
                __cpuidex( cpuInfo.data(), 7, 0 );
                f_7_EBX_ = static_cast<unsigned int>(cpuInfo[1]);
                f_7_ECX_ = static_cast<unsigned int>(cpuInfo[2]);
            }

            __cpuidex( cpuInfo.data(), 0x80000000, 0 );
            nExIds_ = static_cast<unsigned int>(cpuInfo[0]);
            if ( nExIds_ >= 0x80000001u ) {
                __cpuidex( cpuInfo.data(), 0x80000001, 0 );
                f_81_ECX_ = static_cast<unsigned int>(cpuInfo[2]);
                f_81_EDX_ = static_cast<unsigned int>(cpuInfo[3]);
            }

            if ( nExIds_ >= 0x80000004u ) {
                std::array<char, 49> brand{};
                for ( int index = 0; index < 3; ++index ) {
                    __cpuidex( cpuInfo.data(), 0x80000002 + index, 0 );
                    memcpy( brand.data() + index * sizeof( cpuInfo ), cpuInfo.data(), sizeof( cpuInfo ) );
                }
                brand_ = brand.data();
            }
        };

        unsigned int nIds_;
        unsigned int nExIds_;
        std::string vendor_;
        std::string brand_;
        bool isIntel_;
        bool isAMD_;
        std::bitset<32> f_1_ECX_;
        std::bitset<32> f_1_EDX_;
        std::bitset<32> f_7_EBX_;
        std::bitset<32> f_7_ECX_;
        std::bitset<32> f_81_ECX_;
        std::bitset<32> f_81_EDX_;
    };
};

// Initialize static member data
const InstructionSet::InstructionSet_Internal InstructionSet::CPU_Rep;

// Print out supported instruction set extensions
void LogInstructionSet() {
    auto support_message = []( std::string isa_feature, bool is_supported ) {
        LogInfo() << isa_feature << (is_supported ? " supported" : " not supported");
    };

    LogInfo() << "[" + InstructionSet::Vendor() + "] " + InstructionSet::Brand();

    support_message( "AVX", InstructionSet::AVX() );
    support_message( "AVX2", InstructionSet::AVX2() );
    support_message( "SSE", InstructionSet::SSE() );
    support_message( "SSE2", InstructionSet::SSE2() );
    support_message( "XSAVE", InstructionSet::XSAVE() );
}

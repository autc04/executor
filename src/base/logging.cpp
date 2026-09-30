#include <base/logging.h>
#include <iomanip>
#include <cctype>

using namespace Executor;

int logging::nestingLevel = 0;
static bool loggingEnabled = false;

void logging::resetNestingLevel()
{
    nestingLevel  = 0;
}
void logging::indent()
{
    for(int i = 0; i < nestingLevel; i++)
        std::clog << "  ";
}

bool logging::enabled()
{
    return loggingEnabled;
}

void logging::setEnabled(bool e)
{
    loggingEnabled = e;
}

bool logging::loggingActive()
{
    return nestingLevel <= 1;
}

void logging::logEscapedCharTo(std::ostream& os, unsigned char c)
{
    if(c == '\'' || c == '\"' || c == '\\')
        os << '\\' << c;
    else if(std::isprint(c))
        os << c;
    else
        os << "\\0" << std::oct << (unsigned)c << std::dec;
}

void logging::logEscapedChar(unsigned char c)
{
    logEscapedCharTo(std::clog, c);
}

bool logging::canConvertBack(const void* p)
{
    if(!p || p == (const void*) -1)
        return true;
#if SIZEOF_CHAR_P == 4 && !defined(TWENTYFOUR_BIT_ADDRESSING)
    bool valid = true;
#else
    bool valid = false;
    for(int i = 0; i < OFFSET_TABLE_SIZE; i++)
    {
        if((uintptr_t)p >= ROMlib_offsets[i] &&
            (uintptr_t)p < ROMlib_offsets[i] + ROMlib_sizes[i])
            valid = true;
    }
#endif
    return valid;
}

bool logging::validAddress(const void* p)
{
    if(!p)
        return false;
    if( (uintptr_t)p & 1 )
        return false;
#if SIZEOF_CHAR_P == 4 && !defined(TWENTYFOUR_BIT_ADDRESSING)
    bool valid = true;
#else
    bool valid = false;
    for(int i = 0; i < OFFSET_TABLE_SIZE; i++)
    {
        if((uintptr_t)p >= ROMlib_offsets[i] &&
            (uintptr_t)p < ROMlib_offsets[i] + ROMlib_sizes[i])
            valid = true;
    }
#endif
    if(!valid)
        return false;

    return true;
}

bool logging::validAddress(syn68k_addr_t p)
{
    if(p == 0 || (p & 1))
        return false;
    return validAddress(SYN68K_TO_US(p));
}


void logging::logValueTo(std::ostream& os, char x)
{
    os << (int)x << " = '";
    logEscapedCharTo(os, x);
    os << '\'';
}
void logging::logValueTo(std::ostream& os, unsigned char x)
{
    os << (int)x;
    if(std::isprint(x))
        os << " = '" << x << '\'';
}
void logging::logValueTo(std::ostream& os, signed char x)
{
    os << (int)x;
    if(std::isprint((unsigned char)x))
        os << " = '" << (char)x << '\'';
}
void logging::logValueTo(std::ostream& os, bool x)
{
    os << (x ? 1 : 0);
}
void logging::logValueTo(std::ostream& os, int16_t x) { os << x; }
void logging::logValueTo(std::ostream& os, uint16_t x) { os << x; }
void logging::logValueTo(std::ostream& os, int32_t x)
{
    os << x << " = '";
    logEscapedCharTo(os, (x >> 24) & 0xFF);
    logEscapedCharTo(os, (x >> 16) & 0xFF);
    logEscapedCharTo(os, (x >> 8) & 0xFF);
    logEscapedCharTo(os, x & 0xFF);
    os << "'";
}
void logging::logValueTo(std::ostream& os, uint32_t x)
{
    os << x << " = '";
    logEscapedCharTo(os, (x >> 24) & 0xFF);
    logEscapedCharTo(os, (x >> 16) & 0xFF);
    logEscapedCharTo(os, (x >> 8) & 0xFF);
    logEscapedCharTo(os, x & 0xFF);
    os << "'";
}
void logging::logValueTo(std::ostream& os, int64_t x) { os << x; }
void logging::logValueTo(std::ostream& os, uint64_t x) { os << x; }
void logging::logValueTo(std::ostream& os, float x) { os << x; }
void logging::logValueTo(std::ostream& os, double x) { os << x; }

namespace
{
void logPascalString(std::ostream& os, const unsigned char* p)
{
    os << "0x" << std::hex << US_TO_SYN68K_CHECK0_CHECKNEG1(p) << std::dec;
    if(logging::validAddress(p) && logging::validAddress(p + 256))
    {
        os << " = \"\\p";
        for(int i = 1; i <= p[0]; i++)
            logging::logEscapedCharTo(os, p[i]);
        os << '"';
    }
}
}
void logging::logValueTo(std::ostream& os, unsigned char* p) { logPascalString(os, p); }
void logging::logValueTo(std::ostream& os, const unsigned char* p) { logPascalString(os, p); }
void logging::logValueTo(std::ostream& os, const void* p)
{
    if(canConvertBack(p))
        os << "0x" << std::hex << US_TO_SYN68K_CHECK0_CHECKNEG1(p) << std::dec;
    else
        os << "?";
}
void logging::logValueTo(std::ostream& os, void* p)
{
    logValueTo(os, (const void*)p);
}
void logging::logValueTo(std::ostream& os, ProcPtr p)
{
    if(canConvertBack(p))
        os << "0x" << std::hex << US_TO_SYN68K_CHECK0_CHECKNEG1(p) << std::dec;
    else
        os << "?";
}

void logging::logValue(char x) { logValueTo(std::clog, x); }
void logging::logValue(unsigned char x) { logValueTo(std::clog, x); }
void logging::logValue(signed char x) { logValueTo(std::clog, x); }
void logging::logValue(bool x) { logValueTo(std::clog, x); }
void logging::logValue(int16_t x) { logValueTo(std::clog, x); }
void logging::logValue(uint16_t x) { logValueTo(std::clog, x); }
void logging::logValue(int32_t x) { logValueTo(std::clog, x); }
void logging::logValue(uint32_t x) { logValueTo(std::clog, x); }
void logging::logValue(unsigned char* p) { logValueTo(std::clog, p); }
void logging::logValue(const void* p) { logValueTo(std::clog, p); }
void logging::logValue(void* p) { logValueTo(std::clog, p); }
void logging::logValue(ProcPtr p) { logValueTo(std::clog, p); }


void logging::dumpRegsAndStack()
{
    std::clog << std::hex << /*std::showbase <<*/ std::setfill('0');
    std::clog << "D0=" << std::setw(8) << EM_D0 << " ";
    std::clog << "D1=" << std::setw(8) << EM_D1 << " ";
    std::clog << "A0=" << std::setw(8) << EM_A0 << " ";
    std::clog << "A1=" << std::setw(8) << EM_A1 << " ";
    //std::clog << std::noshowbase;
    std::clog << "Stack: ";
    uint8_t *p = (uint8_t*)SYN68K_TO_US(EM_A7);
    for(int i = 0; i < 12 && validAddress(p+i); i++)
        std::clog << std::setfill('0') << std::setw(2) << (unsigned)p[i] << " ";
    std::clog << std::dec;
}

void logging::logUntypedArgs(const char *name)
{
    if(loggingActive())
    {
        std::clog.clear();
        indent();
        std::clog << name << " ";
        dumpRegsAndStack();
        std::clog << std::endl;
    }
}
void logging::logUntypedReturn(const char *name)
{
    if(loggingActive())
    {
        indent();
        std::clog << "returning: " << name << " ";
        dumpRegsAndStack();
        std::clog << std::endl << std::flush;
    }
}

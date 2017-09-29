#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <utility/io.hpp>
#include <utility/macros.hpp>
#include <utility/oneof.hpp>

/*
    Paper #1
    ========

    Final list/spreadsheet (CSV export) sorted by cost (L->H, H->L).  I will have a certain price range / budget.  Manually compare all motors in that price range.
    Useful values to display: output power per gram (W/g), output power per dollar (W/$), input power (W, HP), input amperage (A), input voltage (V), efficiency (%), battery life (min), weight (g)
     - Also calculated: maximum speed at various inclines (graph?) w/ maximum incline

    Find mΩ spec for more motors
    - Steal _MotoCalc_ (not my app) data
    - Add brushed motors

    Cost includes shipping for all items - per-site constants/equations

    Sort - use __gnu_parallel::sort() from <parallel/algorithm> + add "-fopenmp" (or regular sort - benchmarks?)
    - Remove bad motors by sort + compare for only Pout & $, all other ratios are for manual comparison


    Paper #2
    ========

    (Match ESC + battery first to make list of max values of I (current))

    To remove pointlessly bad motors:
    - Find each motor's Pout, $, and Pout/$, for each Imax step from each ESC + battery combo
      - Only add new, untested combinations of motors in the case of partial update (new E+B per M and new M per E+B)
    - Remove all where F() or G() holds true
      - How? Sort by W or $, then delete all where traveling in worse direction, checking with corresponding function (F() or G())
    - Use F or G based on benchmark or compiler output (try all 4 combinations)
      - Create new equations if NAND exists

    Base equations (same meaning):
        1) Wcurr <= Wanother && $curr >= $another
           && (Wcurr <= Wanother || $curr > $another) // W is equal or worse and $ is equal or worse, and at least one is worse
        2) (Wcurr <= Wanother && $curr > Wanother)
           || (Wcurr < Wanother && $curr >= $another) // W is equal or worse and $ is worse, or W is worse and $ is equal or worse - MAKES MORE SENSE, USE

    F(where KnowIsGorE == $) { Wcurr < Wanother || (Wcurr == Wanother && $curr >  $another) } // Final, use bool ops also
    G(where KnowIsLorE == W) { $curr > Wanother || (Wcurr <  Wanother && $curr >= $another) } // Final, use bool ops also

    Function simulation:
    Sumulation set (ss) (x=$, y=W): {(2, 15), (3, 12), (1, 13), (7, 19), (9, 17), (2, 14)} // Array aligned to cache line in code?
    End goal       (end):           {(1, 13), (2, 15), (7, 19)}

    DEFINE: Sort1(by x/$), Sort2(by y/W) // In either direction, whichever is faster (sort, then test/traverse)

    // Both could be reversed, within X's gets removed based on F/G run in order of respective x/y getting worse - compare triangularly, i.e. skip comparing if was used as baseline previously
    Sort1(ss): {(1, 13), [(2, 15), X(2, 14)X], X(3, 12)X, (7, 19), X(9, 17)X} // Within []: could be swapped unless stable, could be reversed
    Sort2(ss): {X(3, 12)X, (1, 13), X(2, 14)X, (2, 15), X(9, 17)X, (7, 19)} // Could be reversed

    ====================

    Important motor factors:
    1) Output power and efficiency at stepped current levels
    2) Battery + ESC cost cost for each current level
    3) ???
    4) Motor cost

    In the end, mech watts per $, in both efficient and max power modes (checking all between brute-force?), with or without a DC converter, at different levels of max battery life.
*/

using FloatType = long double;

// TODO: Support different input variables; e.g. no-load RPM instead of Kv, or specs without no load RPM but with max output power, or
//       specs with torque at stall and max efficiency but no resistance
// TODO: Lipo cell count to voltage -- some sites specify that
// TODO: If the user doesn't know something like no-load current or resistance, then warn in yellow about inaccuracies, and don't calculate anything
//       depending on efficiency.
// TODO: Handling unloaded current at a different voltage?  I see that everywhere.  Possible?  V=I/R?
// TODO: Y/N shouldn't need 'enter' to be pressed afterwards.

#define CHECK_AND_PAUSE                            \
do {                                               \
    int result;                                    \
    if((result = pause()) < 0)                     \
        return errno == 0 ? EXIT_FAILURE : -errno; \
    else if(result == 1)                           \
        /*goto restart;*/                          \
} while(false)

// TODO: How to restart?  Longjump?  exec?  goto?  Combine strategies?  Look for security, reliability, and performance here.

static void clearCin()
{
    std::cin.clear();
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}

template <typename T, bool NoZero = false>
T requestInput(std::string const &what)
{
    std::size_t printedLines = 0;

    if(!std::is_same<T, bool>())
        std::cout << std::defaultfloat << std::setprecision(std::numeric_limits<FloatType>::digits10);

    auto displayResult = [&printedLines, &what](auto value)
    {
        std::cout << "\x1b[" << printedLines << "A\x1b[J"; // Move up printedLines lines and clear down
        printedLines = 0;

        auto const pos = (what.front() == '\x1b') ? what.find('m') + 1 : 0;
        std::string const newWhat = what.substr(0, pos) + scast<char>(toupper(what[pos])) + what.substr(pos + 1);

        std::cout << newWhat << ": ";

        // XXX: Cast to base type of insertion possible?
        if(std::is_same<std::decay_t<decltype(value)>, bool>())
            std::cout << (value ? "✓" : "X");
        else
            std::cout << value;

        std::cout << '\n';
        return value;
    };

    while(true)
    {
        ++printedLines; // We're going to print a new line of text
        if(std::is_same<T, bool>())
        {
            std::cout << what << " [y/n]: \x1b[s";

            while(true)
            {
                std::string yesNo;
                std::getline(std::cin, yesNo);

                if(std::cin.fail())
                    clearCin();
                else if(yesNo.empty()) // Handle enter with no text
                {
                    std::cout << "\x1b[A\x1b[u"; // Restore cursor // TODO: Sometimes fails???
                    continue;
                } else {
                    std::transform(yesNo.begin(), yesNo.end(), yesNo.begin(), tolower);
                    if(yesNo == zutil::oneOf("yes", "y", "no", "n"))
                        return displayResult(yesNo[0] == 'y'); // true if yes
                }

                break;
            }
        } else {
            std::cout << "Enter " << what << ": \x1b[s"; // Save cursor

            while(true)
            {
                std::string str;
                std::getline(std::cin, str);

                if(std::cin.fail())
                    clearCin();
                else if(str.empty()) // Handle enter with no text
                {
                    std::cout << "\x1b[A\x1b[u"; // Restore cursor
                    continue;
                } else {
                    std::stringstream ss(str, std::ios::in);
                    T var;
                    ss >> var;

                    if(!ss.fail() && (!std::is_arithmetic<T>() || (NoZero ? var > 0 : var >= 0)))
                        return displayResult(var);
                }

                break;
            }
        }

        std::cout << "\x1b[31mInvalid entry, try again.\x1b[0m\n";
        ++printedLines;
    }
}

template <typename T, bool NoZero = false>
bool confirmAndRequestInput(T &var, std::string const name)
{
    return requestInput<bool>("Do you know "s + name + "?") ?
        (var = requestInput<std::decay_t<T>, NoZero>(name), true)
      : false;
}

struct ValueSet
{
    // ---------- Inputs ----------
    // kv = 1352.l / kt OR (rpm@V) / (V - (I@rpm - armatureR/1000))
    FloatType const kv            = requestInput<FloatType, true>("\x1b[1m" "Kv"                       "\x1b[0m");
    // kt = Q@rpm / ((I@rpm-I@idle)*.007...) where Q =/= 0
    auto        const kt            = 1352.l / kv * .00706155181422604375l;
    FloatType const voltage       = []()
    {
        FloatType input;
        if(confirmAndRequestInput<FloatType, true>(input, "\x1b[1m" "voltage (V)" "\x1b[0m"))
            return input;
        if(confirmAndRequestInput<FloatType, true>(input, "\x1b[1m" "equiv. # of LiPo cells" "\x1b[0m"))
            return input * 3.7l; // 3.7 volt nominal LiPo voltage
        // TODO: Set custom parenthesized equivalent as part of `requestInput`, e.g. "(x.xx volts)" here?

        std::cout << "Uh-oh, you don't have enough info, or this calculator sucks!\n\n\n";
        pause();
        std::exit(EXIT_FAILURE); // TODO: Restart choice should actually work here
    }();
    // I@idle = I@Q - Q / (kt*.007...)
    FloatType const noLoadCurrent = requestInput<FloatType>      ("\x1b[1m" "unloaded current (A)"     "\x1b[0m");
    // maxCurrent is (should be) optional
    FloatType       maxCurrent    = requestInput<FloatType, true>("\x1b[1m" "maximum current (A)"      "\x1b[0m");
    // armatureR = 1000 * ((V - rpm/kv) / I@rpm)
    FloatType const armatureR     = requestInput<FloatType>      ("\x1b[1m" "armature resistance (mΩ)" "\x1b[0m");

    // TODO: Advanced calculation of missing values
    // TODO: (Global) Graph it!

    auto motorPower(FloatType torque) // torque in Nm (for now?) // Q@Pout = (60 * Pout) / (2 * pi * rpm) ; Q@I = kt * (I - I@idle) * .007...
    {
        auto current = torque / kt + noLoadCurrent; // I@rpm = (1000 * (V - rpm/kv)) / armatureR
        auto rpm     = kv * (voltage - (current * armatureR / 1000.l)); // Voltage drop from resistance accounted for
        return torque * rpm * LD_PI / 30.l; // (XXX: @LanguageIssue) 2 * pi / 60 -> pi / 30
    }
};

enum class ValToFind: uint8_t { Power, Efficiency };

#define GLUE_(x, y) x##y
#define GLUE( x, y) GLUE_(x,    y)
#define LD_PI       GLUE( M_PI, L)

// TODO: Remove code duplication.
[[gnu::pure]] static FloatType findMax(Inputs const &values, ValToFind val)
{
    // Calculus FTW!
    return val == ValToFind::Power ?      (values.noLoadCurrent + (1000.l * values.voltage / values.armatureR)) / 2.l
                                   : sqrtl(values.noLoadCurrent * (1000.l * values.voltage / values.armatureR));
}

#define RET_ON_ERR(...) ({ auto res = __VA_ARGS__; if(res < 0) return res; })

// TODO: Real error message upon failure of pause()
int pause()
{
    std::cout << "Press [Esc] to quit or [Enter] to restart... \n";

    struct termios oldt;
    RET_ON_ERR(tcgetattr(STDIN_FILENO, &oldt));

    {
        struct termios newt = oldt; newt.c_lflag &= ~(ICANON | ECHO);
        RET_ON_ERR(tcsetattr(STDIN_FILENO, TCSANOW, &newt));
    }

    int  result;
    char ch = '\0';

    do {
        result = zutil::smartRead(STDIN_FILENO, &ch, 1);
        if(ch == zutil::oneOf('\x1b', '\n'))
        {
            // XXX: @LanguageIssue Candidate for "defer" or a destructor
            RET_ON_ERR(tcsetattr(STDIN_FILENO, TCSANOW, &oldt));
            return ch == '\n';
        }
    } while(result >= 0);

    RET_ON_ERR(tcsetattr(STDIN_FILENO, TCSANOW, &oldt));
    return result;
}

// XXX: @LanguageAlt fn main(string[] argv) or fn main(string[] argv) -> num/int/s32 (return type deduced -- int has a different meaning here)
int main(int const argc, char ** const argv)
{
    // XXX: @LanguageIssue isatty() should not fail.  To lazy to SMART_ASSERT() on errno.
    switch(isatty(STDIN_FILENO) + isatty(STDOUT_FILENO))
    {
    case 2: break;
    case 1:
        std::cout << "This application is meant to be fully interactive. Starting a terminal...\n";
    [[fallthrough]];
    case 0:
        if(argc >= 2 && ""_sv != argv[1]) != 0) {
            // TODO: Warn about misuse or error
        }

        // We are not connected to a terminal, and we were started directly.  Open a terminal.

        // XXX: This opens konsole on Arch.  Try exo-open first, and put konsole last unless we're on KDE.  Also try using update-alternatives.
        // XXX: Why "p"?
        char const * const termOption1[] = {"x-terminal-emulator", "--title=MotorCalc",        "-x", argv[0], "", nullptr};
        char const * const termOption2[] = {"gnome-terminal",      "-t", "MotorCalc",          "-x", argv[0], "", nullptr};
        //char const * const termOption3[] = {"konsole",             "-p", "tabtitle=MotorCalc", "-e", argv[0], "", nullptr}; // XXX: @Temporary
        char const * const termOption4[] = {"xfce4-terminal",      "-T=MotorCalc",             "-x", argv[0], "", nullptr};
        char const * const termOption5[] = {"xterm",               "-T", "MotorCalc",          "-e", argv[0], "", nullptr};
        char const * const * const termOptions[] = {termOption1, termOption2, /*termOption3,*/ termOption4, termOption5};

        for(auto const termOption: termOptions)
        {
            execvp(const_cast<char *>(termOption[0]), const_cast<char * const *>(termOption));
            // XXX: zutil::exec(termOption); -- how to avoid ambiguity with optional argv[0] for the array-as-args version?
            SMART_ASSERT(errno != 0);

            // TODO: Should we check for a fatal error, retry, or skip here?
            //       Are there other places here, in other programs, this question could apply to?
            if(errno != ENOENT) // An error occurred other then the requested terminal not existsing
                return -errno;
        }

    return EXIT_FAILURE; // Usable terminal could not be found
    default: SMART_ASSERT(false);
    }

    {
        ValueSet values; // Default constructor asks for input

        std::cout << std::setprecision(2) << std::fixed;

        if(values.maxCurrent - values.noLoadCurrent < .01L)
        {
            std::cout << "\n\n\x1b[31mError: Maximum current is less than or very close to unloaded current.\x1b[0m\n\n\n";
            CHECK_AND_PAUSE;
            return EXIT_SUCCESS;
        }

        if((values.noLoadCurrent + .0001L) * values.armatureR / 1000.l > values.voltage)
        {
            std::cout << "\n\n\x1b[31mError: At no load current or barely above, the motor would be an open circuit (Vdrop > Vin).\x1b[0m\n\n\n";
            CHECK_AND_PAUSE;
            return EXIT_SUCCESS;
        }

        if(values.maxCurrent * values.armatureR / 1000.l >= values.voltage)
        {
            values.maxCurrent = values.voltage / (values.armatureR / 1000.l) + .0001L;
            std::cout << "\n\n\x1b[1;33mWarning: At maximum current, the motor would be an open circuit (Vdrop > Vin).\n"
                         "Maximum current has been reduced to \x1b[36m" << values.maxCurrent << " A\x1b[33m.\x1b[0m\n";
        }

        struct Values
        {
            FloatType current, rpm, torque, powerIn, powerOut, efficiency; // TODO: Get some of these values back through member functions

            Values(ValueSet const &values, FloatType const currentIn): current(currentIn)
            {
                powerOut   = values.motorPower(current);
                powerIn    = values.voltage * current;
                efficiency = powerOut * 100.l / powerIn;
            }

            void print()
            {
                // XXX: @LaunguageIssue Manual columnification.  Existing iostream utilities (setw + math) might be a way to implement auto-columns.
                std::cout << "Current:    \x1b[1;36m" << current      << " A"   "\x1b[0m\n"
                             "Speed:      \x1b[1;36m" << rpm          << " RPM" "\x1b[0m\n"
                             "Torque:     \x1b[1;36m" << torque * 100 << " Ncm" "\x1b[0m\n"
                             "Power in:   \x1b[1;36m" << powerIn      << " W"   "\x1b[0m\n"
                             "Power out:  \x1b[1;36m" << powerOut     << " W\x1b[0m, \x1b[1;36m" << powerOut / 745.69987158227022L << " HP\x1b[0m\n"
                             "Efficiency: \x1b[1;36m" << efficiency   << "%"    "\x1b[0m\n";
            }
        } maxPower(values, findMax(values, ValToFind::Power)), maxEfficiency(values, findMax(values, ValToFind::Efficiency));

        std::cout << "\n\n\x1b[1mAt maximum output power:\x1b[0m\n";
        maxPower.print();
        std::cout << "\n\n\x1b[1mAt maximum efficiency:\x1b[0m\n";
        maxEfficiency.print();
        std::cout << "\n\n";
    }

    CHECK_AND_PAUSE;
    return EXIT_SUCCESS;
}

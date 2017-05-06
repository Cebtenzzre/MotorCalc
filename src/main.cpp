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

// TODO: Support different input variables; e.g. no-load RPM instead of Kv, or specs without no load RPM but with max output power, or
//       specs with torque at stall and max efficiency but no resistance

#define CHECK_AND_PAUSE                            \
do {                                               \
    int result;                                    \
    if((result = pause()) < 0)                     \
        return errno == 0 ? EXIT_FAILURE : -errno; \
    else if(result == 1)                           \
        goto restart;                              \
} while(false)

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
        std::cout << std::defaultfloat << std::setprecision(std::numeric_limits<long double>::digits10);

    auto displayResult = [&printedLines, &what] (auto const value)
    {
        std::cout << "\x1b[" << printedLines << "A\x1b[J"; // Move up printedLines lines and clear down
        printedLines = 0;

        auto const pos = (what.front() == '\x1b') ? what.find('m') + 1 : 0;
        std::string const newWhat = what.substr(0, pos) + scast<char>(toupper(what[pos])) + what.substr(pos + 1);

        std::cout << newWhat << ": ";

        if(std::is_same<decltype(value), bool>())
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
            std::cout << what << " [Y/n]: ";

            std::string yesNo;
            std::getline(std::cin, yesNo);

            if(std::cin.fail())
                clearCin();
            else {
                std::transform(yesNo.begin(), yesNo.end(), yesNo.begin(), tolower);
                if(yesNo == utility::oneOf("yes", "y", "", "no", "n"))
                    return displayResult(yesNo[0] == utility::oneOf('y', '\0')); // true if yes
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

#if 0
template <typename T>
bool confirmAndRequestInput(T &var, std::string const name)
{
    auto const known = requestInput<bool>(std::string("Do you know ") + name + "?");
    if(known)
        var = requestInput<std::remove_reference_t<T>>(name);

    return known;
}
#endif

struct Inputs
{
    long double const kv            = requestInput<long double, true>("\x1b[1m" "Kv"                       "\x1b[0m");
    long double const voltage       = requestInput<long double, true>("\x1b[1m" "voltage"                  "\x1b[0m");
    long double const noLoadCurrent = requestInput<long double>      ("\x1b[1m" "unloaded current (A)"     "\x1b[0m");
    long double       maxCurrent    = requestInput<long double, true>("\x1b[1m" "maximum current (A)"      "\x1b[0m");
    long double const armatureR     = requestInput<long double>      ("\x1b[1m" "armature resistance (mΩ)" "\x1b[0m");
};

enum class ValToFind: uint8_t
{
    Power,
    Efficiency
};

#define GLUE_(x, y) x##y
#define GLUE(x, y) GLUE_(x, y)
#define LD_PI GLUE(M_PI, L)

// TODO: Remove code duplication.
[[gnu::pure]] static long double findMax(Inputs const &inputs, ValToFind const val)
{
    auto const kt = 1352.L / inputs.kv;

    long double const hardMinCurrent = inputs.noLoadCurrent + .0001L; // To avoid zero/very low torque
    long double minCurrent = hardMinCurrent;
    long double maxCurrent = inputs.maxCurrent;
    long double step = (inputs.maxCurrent - hardMinCurrent) / 10L;
    long double best = 0.L, bestCurrent = hardMinCurrent; // Highest value and the current at which it is reached

    do {
        bool update = false;

        for(long double current = minCurrent; current <= maxCurrent;
            ((current += step) > maxCurrent && (current - step) < maxCurrent /* Don't run forever with current at max */) ?
            current = maxCurrent : 0)
        {
            long double const rpm = (inputs.voltage - current * inputs.armatureR / 1000.L) * inputs.kv;
            long double const q   = kt * (current - inputs.noLoadCurrent) * .00706L; // Q in in ozf-in converted to Nm

            long double const powerOut   = q * rpm * LD_PI / 30.L; // 2 * pi / 60 = pi / 30

            if(val == ValToFind::Power)
            {
                if(powerOut > best)
                {
                    update      = true;
                    best        = powerOut;
                    bestCurrent = current;
                }
            } else {
                long double const powerIn    = inputs.voltage * current;
                long double const efficiency = (powerOut / powerIn) * 100.L;

                if(efficiency > best)
                {
                    update      = true;
                    best        = efficiency;
                    bestCurrent = current;
                }
            }
        }

        if(!update) // No changes, we have hit a maximum or something screwy happened with small/big numbers
            break;

        // Range of bestCurrent +/- step, limited between hardMinCurrent and inputs.maxCurrent
        minCurrent = std::max(bestCurrent - step, hardMinCurrent);
        maxCurrent = std::min(bestCurrent + step, inputs.maxCurrent);

        step /= 10;
    } while(utility::oneOf(maxCurrent - bestCurrent, bestCurrent - minCurrent) >= .0001L); // Accurate to 4 decimal places, or +/- .0001

    return bestCurrent;
}

// TODO: Real error message upon failure of pause()
int pause()
{
    std::cout << "Press [Esc] to quit or [Enter] to restart... \n";

    struct termios oldt;
    tcgetattr(STDIN_FILENO, &oldt); // XXX: @Robustness Warn on failure.

    errno = 0;

    int result;

    {
        struct termios newt = oldt;
        newt.c_lflag &= ~(ICANON | ECHO);

        if((result = tcsetattr(STDIN_FILENO, TCSANOW, &newt)) < 0)
            return result;
    }

    char ch = '\0';

    do {
        result = utility::smartRead(STDIN_FILENO, &ch, 1);
        if(ch == utility::oneOf('\x1b', '\n'))
        {
            tcsetattr(STDIN_FILENO, TCSANOW, &oldt); // XXX: @Robustness Warn on failure.
            return ch == '\n';
        }
    } while(result >= 0);

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt); // XXX: @Robustness Warn on failure.

    return result;
}

int main([[maybe_unused]] int const argc, char ** const argv)
{
    if(utility::oneOf(isatty(STDIN_FILENO), isatty(STDOUT_FILENO)) == 0)
    {
        // We are not connected to a terminal, and probably were started directly.  Open a terminal.

        // XXX: This opens konsole on Arch.  Try exo-open first, and put konsole last unless we're on KDE.
        char const * const termOption1[] = {"x-terminal-emulator", "--title=MotorCalc",        "-x", argv[0], "p", nullptr};
        char const * const termOption2[] = {"gnome-terminal",      "-t", "MotorCalc",          "-x", argv[0], "p", nullptr};
        //char const * const termOption3[] = {"konsole",             "-p", "tabtitle=MotorCalc", "-e", argv[0], "p", nullptr}; // XXX: @Temporary
        char const * const termOption4[] = {"xfce4-terminal",      "-T=MotorCalc",             "-x", argv[0], "p", nullptr};
        char const * const termOption5[] = {"xterm",               "-T", "MotorCalc",          "-e", argv[0], "p", nullptr};
        char const * const * const termOptions[] = {termOption1, termOption2, /*termOption3,*/ termOption4, termOption5};

        for(auto const termOption: termOptions)
        {
            execvp(const_cast<char *>(termOption[0]), const_cast<char * const *>(termOption));

            // TODO: Should we check for a fatal error, retry, or skip here?
            //       Are there other places here, in other programs, this question could apply to?
            if(errno != ENOENT) // An error occurred other then the requested terminal not existsing
                return errno == 0 ? EXIT_FAILURE : -errno;
        }

        return EXIT_FAILURE; // Usable terminal could not be found
    }

    restart:

    {
        Inputs inputs; // Default constructor asks for input

        std::cout << std::setprecision(2) << std::fixed;

        if(inputs.maxCurrent - inputs.noLoadCurrent < .01L)
        {
            std::cout << "\n\n\x1b[31mError: Maximum current is less than or very close to unloaded current.\x1b[0m\n\n\n";
            CHECK_AND_PAUSE;
            return EXIT_SUCCESS;
        }

        if((inputs.noLoadCurrent + .0001L) * inputs.armatureR / 1000.L > inputs.voltage)
        {
            std::cout << "\n\n\x1b[31mError: At no load current or barely above, the motor would be an open circuit (Vdrop > Vin).\x1b[0m\n\n\n";
            CHECK_AND_PAUSE;
            return EXIT_SUCCESS;
        }

        if(inputs.maxCurrent * inputs.armatureR / 1000.L >= inputs.voltage)
        {
            inputs.maxCurrent = inputs.voltage / (inputs.armatureR / 1000.L) + .0001L;
            std::cout << "\n\n\x1b[1;33mWarning: At maximum current, the motor would be an open circuit (Vdrop > Vin).\n"
                         "Maximum current has been reduced to \x1b[36m" << inputs.maxCurrent << " A\x1b[33m.\x1b[0m\n";
        }

        struct Values
        {
            long double current, rpm, q, powerIn, powerOut, efficiency;

            Values(Inputs const &inputs, long double const currentIn): current(currentIn)
            {
                auto const kt = 1352.L / inputs.kv;

                rpm = (inputs.voltage - current * inputs.armatureR / 1000.L) * inputs.kv;
                q   = kt * (current - inputs.noLoadCurrent) * .00706L; // Q in in ozf-in converted to Nm

                powerOut   = q * rpm * LD_PI / 30.L; // 2 * pi / 60 = pi / 30
                powerIn    = inputs.voltage * current;
                efficiency = (powerOut / powerIn) * 100.L;
            }

            void print()
            {
                // XXX: @LaunguageIssue Manual columnification.  Existing iostream utilities (setw + math) might be a way to implement auto-columns.
                std::cout << "Current:    \x1b[1;36m" << current    << " A"   "\x1b[0m\n"
                             "Speed:      \x1b[1;36m" << rpm        << " RPM" "\x1b[0m\n"
                             "Torque:     \x1b[1;36m" << q * 100    << " Ncm" "\x1b[0m\n"
                             "Power in:   \x1b[1;36m" << powerIn    << " W"   "\x1b[0m\n"
                             "Power out:  \x1b[1;36m" << powerOut   << " W\x1b[0m, \x1b[1;36m" << powerOut / 745.69987158227022L << " HP\x1b[0m\n"
                             "Efficiency: \x1b[1;36m" << efficiency << "%"    "\x1b[0m\n";
            }
        } maxPower(inputs, findMax(inputs, ValToFind::Power)), maxEfficiency(inputs, findMax(inputs, ValToFind::Efficiency));

        std::cout << "\n\n\x1b[1mAt maximum output power:\x1b[0m\n";
        maxPower.print();
        std::cout << "\n\n\x1b[1mAt maximum efficiency:\x1b[0m\n";
        maxEfficiency.print();
        std::cout << "\n\n";
    }

    CHECK_AND_PAUSE;
    return EXIT_SUCCESS;
}

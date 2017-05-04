#define _USE_MATH_DEFINES
    #include <cmath>
#undef _USE_MATH_DEFINES

#include <cctype>
#include <cerrno>
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

#define CHECK_AND_PAUSE                            \
do {                                               \
    int result;                                    \
    if((result = pause()) < 0)                     \
        return errno == 0 ? EXIT_FAILURE : -errno; \
    else if(result == 1)                           \
        goto restart;                              \
} while(false)

template <typename T, bool NoZero = false>
T requestInput(std::string const &what)
{
    std::size_t printedLines = 0;

    std::cout << std::defaultfloat << std::setprecision(std::numeric_limits<double>::digits10 + 2);

    while(true)
    {
        if(std::is_same<T, bool>())
        {
            std::cout << what << " [Y/n]: ";
            ++printedLines;

            std::string yesNo;
            std::getline(std::cin, yesNo);

            if(std::cin.fail())
            {
                std::cin.clear();
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            } else {
                std::transform(yesNo.begin(), yesNo.end(), yesNo.begin(), tolower);
                if(yesNo == utility::oneOf("yes", "y", ""))
                {
                    std::cout << "\x1b[" << printedLines << "A\x1b[J"; // Move up printedLines lines and clear down
                    printedLines = 0;

                    std::cout << scast<char>(std::toupper(what.front())) << what.substr(1) << ": ✓\n";
                    return true;
                }
                if(yesNo == utility::oneOf("no", "n"))
                {
                    std::cout << "\x1b[" << printedLines << "A\x1b[J"; // Move up printedLines lines and clear down
                    printedLines = 0;

                    std::cout << scast<char>(std::toupper(what.front())) << what.substr(1) << ": X\n";
                    return false;
                }
            }
        } else {
            std::cout << "Enter " << what << ": \x1b[s"; // Save cursor
            ++printedLines;

            inPlaceRetry:
            std::string str;
            std::getline(std::cin, str);

            if(std::cin.fail())
            {
                std::cin.clear();
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            } else if(str.empty()) // Handle enter with no text
            {
                std::cout << "\x1b[u"; // Restore cursor
                goto inPlaceRetry;
            } else {
                std::stringstream ss(str, std::ios::in);
                T var;
                ss >> var;

                if(!ss.fail() && (!std::is_arithmetic<T>() || (NoZero ? var > 0 : var >= 0)))
                {
                    std::cout << "\x1b[" << printedLines << "A\x1b[J"; // Move up printedLines lines and clear down
                    printedLines = 0;

                    std::cout << scast<char>(toupper(what.front())) << what.substr(1) << ": " << var << "\n";
                    return var;
                }
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
    double const kv            = requestInput<double, true>("Kv");
    double const voltage       = requestInput<double, true>("voltage");
    double const noLoadCurrent = requestInput<double>      ("unloaded current (A)");
    double       maxCurrent    = requestInput<double, true>("maximum current (A)");
    double const armatureR     = requestInput<double>      ("armature resistance (mΩ)");
};

enum class ValToFind: uint8_t
{
    Power,
    Efficiency
};

[[gnu::pure]] double findMax(Inputs const &inputs, ValToFind const val)
{
    double const kt = 1352 / inputs.kv;

    double const hardMinCurrent = inputs.noLoadCurrent + 0.0001; // To avoid zero/very low torque
    double minCurrent = hardMinCurrent;
    double maxCurrent = inputs.maxCurrent;
    double step = (inputs.maxCurrent - hardMinCurrent) / 10;
    double best = 0, bestCurrent = hardMinCurrent; // Highest value and the current at which it is reached

    do {
        bool update = false;

        for(double current = minCurrent; current <= maxCurrent;
            ((current += step) > maxCurrent && (current - step) < maxCurrent /* Don't run forever with current at max */) ?
            current = maxCurrent : 0)
        {
            double const rpm = (current * inputs.armatureR / 1000 - inputs.voltage) * inputs.kv * -1;
            double const q   = kt * (current - inputs.noLoadCurrent) * 0.00706; // Q in in ozf-in converted to Nm

            double const powerOut   = q * rpm * (2 * M_PI) / 60;

            if(val == ValToFind::Power)
            {
                if(powerOut > best)
                {
                    update      = true;
                    best        = powerOut;
                    bestCurrent = current;
                }
            } else {
                double const powerIn    = inputs.voltage * current;
                double const efficiency = (powerOut / powerIn) * 100;

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
    } while(utility::oneOf(maxCurrent - bestCurrent, bestCurrent - minCurrent) >= 0.0001); // Accurate to 4 decimal places, or +/- 0.0001

    return bestCurrent;
}

int pause()
{
    std::cout << "Press [Esc] to quit or [Enter] to restart... \n";

    struct termios oldt;
    tcgetattr(STDIN_FILENO, &oldt);

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
        result = read(STDIN_FILENO, &ch, 1);
        if(ch == utility::oneOf('\x1b', '\n'))
        {
            tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
            return ch == '\x1b';
        }
    } while(result >= 0);

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);

    return result;
}

int main(int const argc, char ** const argv)
{
    if(utility::oneOf(isatty(STDIN_FILENO),  isatty(STDOUT_FILENO)) == 0)
    {
        // We are not connected to a terminal, and probably were started directly.  Open a terminal.

        // XXX: This opens konsole on Arch.  Try exo-open first, and put konsole last unless we're on KDE.
        char const * const termOption1[] = {"x-terminal-emulator", "--title=MotorCalc",        "-x", argv[0], "p", nullptr};
        char const * const termOption2[] = {"gnome-terminal",      "-t", "MotorCalc",          "-x", argv[0], "p", nullptr};
        char const * const termOption3[] = {"konsole",             "-p", "tabtitle=MotorCalc", "-e", argv[0], "p", nullptr};
        char const * const termOption4[] = {"xfce4-terminal",      "-T=MotorCalc",             "-x", argv[0], "p", nullptr};
        char const * const termOption5[] = {"xterm",               "-T", "MotorCalc",          "-e", argv[0], "p", nullptr};
        char const * const * const termOptions[] = {termOption1, termOption2, termOption3, termOption4, termOption5};

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

        if(inputs.maxCurrent - inputs.noLoadCurrent < 0.01)
        {
            std::cout << "\n\n\x1b[31mError: Maximum current is less than, equal to, or very close to unloaded current.\x1b[0m\n\n\n";
            CHECK_AND_PAUSE;
            return EXIT_SUCCESS;
        }

        if((inputs.noLoadCurrent + 0.0001) * inputs.armatureR / 1000 > inputs.voltage)
        {
            std::cout << "\n\n\x1b[31mError: At minimum current or barely above, the motor would be an open circuit (Vdrop > Vin).\x1b[0m\n\n\n";
            CHECK_AND_PAUSE;
            return EXIT_SUCCESS;
        }

        if(inputs.maxCurrent * inputs.armatureR / 1000 >= inputs.voltage)
        {
            inputs.maxCurrent = inputs.voltage / (inputs.armatureR / 1000) + 0.0001;
            std::cout << "\n\n\x1b[1;33mWarning: At maximum current, the motor would be an open circuit (Vdrop > Vin).\n"
                         "Maximum current has been reduced to \x1b[36m" << inputs.maxCurrent << " A\x1b[33m.\x1b[0m\n";
        }

        auto const pOutCurrent = findMax(inputs, ValToFind::Power);
        auto const effCurrent  = findMax(inputs, ValToFind::Efficiency);

        double const kt = 1352 / inputs.kv;

        double const pOutRpm = (pOutCurrent * inputs.armatureR / 1000 - inputs.voltage) * inputs.kv * -1;
        double const pOutQ   = kt * (pOutCurrent - inputs.noLoadCurrent) * 0.00706; // Q in in ozf-in converted to Nm

        double const pOutPowerOut   = pOutQ * pOutRpm * (2 * M_PI) / 60;
        double const pOutPowerIn    = inputs.voltage * pOutCurrent;
        double const pOutEfficiency = (pOutPowerOut / pOutPowerIn) * 100;


        double const effRpm = (effCurrent * inputs.armatureR / 1000 - inputs.voltage) * inputs.kv * -1;
        double const effQ   = kt * (effCurrent - inputs.noLoadCurrent) * 0.00706; // Q in in ozf-in converted to Nm

        double const effPowerOut   = effQ * effRpm * (2 * M_PI) / 60;
        double const effPowerIn    = inputs.voltage * effCurrent;
        double const effEfficiency = (effPowerOut / effPowerIn) * 100;

        std::cout << "\n\n"
                     "At maximum output power:\n"
                     "\x1b[1;36m" << pOutCurrent    << " A\x1b[0m current\n"
                     "\x1b[1;36m" << pOutRpm        << " RPM\x1b[0m\n"
                     "\x1b[1;36m" << pOutQ * 100    << " Ncm\x1b[0m torque\n"
                     "\x1b[1;36m" << pOutPowerIn    << " W\x1b[0m in\n"
                     "\x1b[1;36m" << pOutPowerOut   << " W\x1b[0m out\n"
                     "\x1b[1;36m" << pOutEfficiency << "%\x1b[0m efficiency\n"
                     "\n\n"
                     "At maximum efficiency:\n"
                     "\x1b[1;36m" << effCurrent    << " A\x1b[0m current\n"
                     "\x1b[1;36m" << effRpm        << " RPM\x1b[0m\n"
                     "\x1b[1;36m" << effQ * 100    << " Ncm\x1b[0m torque\n"
                     "\x1b[1;36m" << effPowerIn    << " W\x1b[0m in\n"
                     "\x1b[1;36m" << effPowerOut   << " W\x1b[0m out\n"
                     "\x1b[1;36m" << effEfficiency << "%\x1b[0m efficiency\n"
                     "\n\n";
    }

    CHECK_AND_PAUSE;
    return EXIT_SUCCESS;
}

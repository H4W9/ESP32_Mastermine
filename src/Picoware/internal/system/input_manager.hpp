#pragma once
#include "Arduino.h"
#include "../../internal/boards.hpp"
#include "../../internal/system/input.hpp"
namespace Picoware
{
    class InputManager
    {
    public:
        InputManager(const Board board) : input(-1), isTouchInput(false), touch(nullptr)
        {
            for (int i = 0; i < 5; i++)
                inputs[i] = nullptr;

            // Touch-panel boards. The backend (FT6336 vs XPT2046) is chosen later
            // by the sketch via TouchInput::attachTFT, not here.
            if (board.boardType == BOARD_TYPE_PANCAKE ||
                board.boardType == BOARD_TYPE_MARAUDER_V8)
            {
                isTouchInput = true;
                touch = new TouchInput(board.width, board.height, board.rotation);
                inputs[0] = new Input(touch);
            }
            else if (board.boardType == BOARD_TYPE_JBLANKED)
            {
                float debounce = 0.05f;
                inputs[0] = new Input(16, BUTTON_UP, debounce);
                inputs[1] = new Input(17, BUTTON_RIGHT, debounce);
                inputs[2] = new Input(18, BUTTON_DOWN, debounce);
                inputs[3] = new Input(19, BUTTON_LEFT, debounce);
                inputs[4] = new Input(20, BUTTON_CENTER, debounce);
            }
        }

        ~InputManager()
        {
            for (int i = 0; i < 5; i++)
            {
                if (inputs[i] != nullptr)
                {
                    delete inputs[i];
                    inputs[i] = nullptr;
                }
            }
            // The Input wrapper does not own `touch`; delete it here (once).
            if (touch != nullptr)
            {
                delete touch;
                touch = nullptr;
            }
        }

        // Reset the input
        inline void reset(bool shouldDelay = false, int delayMs = 150)
        {
            input = -1;
            for (int i = 0; i < 5; i++)
                if (inputs[i] != nullptr)
                    inputs[i]->reset();
            if (shouldDelay)
                delay(delayMs);
        }

        // Run the input manager and check for input events.
        inline void run()
        {
            input = -1;
            if (isTouchInput)
            {
                inputs[0]->run();
                input = inputs[0]->getLastButton();
                return;
            }
            for (int i = 0; i < 5; i++)
            {
                if (inputs[i] != nullptr)
                {
                    inputs[i]->run();
                    if (inputs[i]->getLastButton() != -1)
                    {
                        input = inputs[i]->getButtonAssignment();
                        break;
                    }
                }
            }
        }

        int getInput() const noexcept { return input; }

        // Direct-tap access for the hybrid touch UI (null on non-touch boards).
        TouchInput *getTouch() const noexcept { return touch; }
        bool hasTouch() const noexcept { return isTouchInput; }

    private:
        int input;
        bool isTouchInput;
        TouchInput *touch;
        Input *inputs[5];
    };
}

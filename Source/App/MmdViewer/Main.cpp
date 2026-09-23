#include "App/MmdViewer/WindowsApplication.h"

#include <windows.h>

#include <cstdlib>
#include <exception>
#include <iostream>

int wmain()
{
    try
    {
        MmdLab::WindowsApplication application;
        application.Initialize(GetModuleHandleW(nullptr), SW_SHOWDEFAULT);
        return application.Run();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "MMDLab Viewer initialization failed: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}

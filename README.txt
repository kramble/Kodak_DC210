Kodac DC210 Camera picture download utility (windows command line)

This code is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
See the GNU General Public License for more details.

This is probably reinventing the wheel, but here goes...

I have an ancient Kodak DC210 camera from which I want to download
pictures. Unfortunately the Picture Easy software seems to have
got bit rot and no longer works (well, for me anyway). Trying the
twain drivers, I could only download one picture at a time.

Looking at other utilites, kdcpi looked ideal, but did not work on windows.

So I wrote my own.

Its based on kdcpi-0.0.3, and only handles the DC210 (but should be easy to
extend using the kdcpi code as a model).

Serial I/O code is from http://playground.arduino.cc/Interfacing/CPPWindows

Anyway since I already have a GitHub account, I hereby share this code.

It builds under Visual Studio 2008 C++ command line, you will need to run
the following command (or similar) to setup the compiler environment.
"C:\Program Files\Microsoft Visual Studio 9.0\VC\vcvarsall.bat" x86

There is no makefile, just run build.bat

PS My coding style is "idiosyncratic", to say the least, but being self
taught its a bit late for me to change now. Deal with it.

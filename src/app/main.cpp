#include <app/app.h>

int main(int argc, char* argv[])
{
	App app;

	app.init();
	app.run();
	app.cleanup();

	return 0;
}

#include <stdio.h>

#define UP_S "\eOA"
#define DOWN_S "\eOB"
#define RIGHT_S "\eOC"
#define LEFT_S "\eOD"

#define F2_S "\eOQ"
#define F3_S "\eOR"
#define F10_S "\e[21~"

#define HOME_S "\e[1~"
#define END_S "\e[4~"
#define PGUP_S "\e[5~"
#define PGDOWN_S "\e[6~"


int main() {
	printf(DOWN_S);
	int i;
	for(i=0;i<400;++i)
		printf("%d\tab\n\bc%d\b " DOWN_S UP_S,i,i);
//		printf("%da\nbc%d " ,i,i);
	printf(PGDOWN_S HOME_S "done");
	printf(F10_S);
	return 0;
}

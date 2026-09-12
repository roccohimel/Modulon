int strlen(char *s);
int putchar(int c);
int puts(char *s);

int main(void) {
	int n = strlen("hello world");
	puts("thunks work");
	printf("len=%v\n", n);
	putchar(65);
	putchar(10);
	if(n == 11)
		return 0;
	return 1;
}

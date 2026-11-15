# txt2c.awk - turn a help text file into a NULL-terminated C array of lines.
# Usage: awk -v name=HELP_KEYS -f tools/txt2c.awk help/keys.txt
# Lines starting with '#' are comments and left out.
BEGIN { printf "static const char *const %s[] __attribute__((unused)) = {\n", name }
/^#/ { next }
{
	gsub(/\r$/, "")
	gsub(/\\/, "\\\\")
	gsub(/"/, "\\\"")
	gsub(/\t/, "        ")
	printf "\t\"%s\",\n", $0
}
END { print "\tNULL\n};\n" }

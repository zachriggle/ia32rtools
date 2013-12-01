#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "my_assert.h"
#include "my_str.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))
#define IS(w, y) !strcmp(w, y)

#include "protoparse.h"

static int is_x86_reg_saved(const char *reg)
{
	static const char *nosave_regs[] = { "eax", "edx", "ecx" };
	int nosave = 0;
	int r;

	for (r = 0; r < ARRAY_SIZE(nosave_regs); r++)
		if (strcmp(reg, nosave_regs[r]) == 0)
			nosave = 1;

	return !nosave;
}

static void out_toasm_x86(FILE *f, char *sym, struct parsed_proto *pp)
{
	int must_save = 0;
	int sarg_ofs = 1; // stack offset to args, in DWORDs
	int args_repushed = 0;
	int i;

	for (i = 0; i < pp->argc; i++) {
		if (pp->arg[i].reg != NULL)
			must_save |= is_x86_reg_saved(pp->arg[i].reg);
	}

	fprintf(f, ".global _%s\n", sym);
	fprintf(f, "_%s:\n", sym);

	if (pp->argc_reg == 0 && !pp->is_stdcall) {
		fprintf(f, "\tjmp %s\n\n", sym);
		return;
	}

	if (pp->argc_stack == 0 && !must_save && !pp->is_stdcall) {
		// load arg regs
		for (i = 0; i < pp->argc; i++) {
			fprintf(f, "\tmovl %d(%%esp), %%%s\n",
				(i + sarg_ofs) * 4, pp->arg[i].reg);
		}
		fprintf(f, "\tjmp %s\n\n", sym);
		return;
	}

	// save the regs
	for (i = 0; i < pp->argc; i++) {
		if (pp->arg[i].reg != NULL && is_x86_reg_saved(pp->arg[i].reg)) {
			fprintf(f, "\tpushl %%%s\n", pp->arg[i].reg);
			sarg_ofs++;
		}
	}

	// reconstruct arg stack
	for (i = pp->argc - 1; i >= 0; i--) {
		if (pp->arg[i].reg == NULL) {
			fprintf(f, "\tmovl %d(%%esp), %%eax\n",
				(i + sarg_ofs) * 4);
			fprintf(f, "\tpushl %%eax\n");
			sarg_ofs++;
			args_repushed++;
		}
	}
	my_assert(args_repushed, pp->argc_stack);

	// load arg regs
	for (i = 0; i < pp->argc; i++) {
		if (pp->arg[i].reg != NULL) {
			fprintf(f, "\tmovl %d(%%esp), %%%s\n",
				(i + sarg_ofs) * 4, pp->arg[i].reg);
		}
	}

	fprintf(f, "\n\t# %s\n", pp->is_stdcall ? "__stdcall" : "__cdecl");
	fprintf(f, "\tcall %s\n\n", sym);

	if (args_repushed && !pp->is_stdcall)
		fprintf(f, "\tadd $%d,%%esp\n", args_repushed * 4);

	// restore regs
	for (i = pp->argc - 1; i >= 0; i--) {
		if (pp->arg[i].reg != NULL && is_x86_reg_saved(pp->arg[i].reg))
			fprintf(f, "\tpopl %%%s\n", pp->arg[i].reg);
	}

	fprintf(f, "\tret\n\n");
}

static void out_fromasm_x86(FILE *f, char *sym, struct parsed_proto *pp)
{
	int sarg_ofs = 1; // stack offset to args, in DWORDs
	int stack_args;
	int i;

	fprintf(f, "# %s\n", pp->is_stdcall ? "__stdcall" : "__cdecl");
	fprintf(f, ".global %s\n", sym);
	fprintf(f, "%s:\n", sym);

	if (pp->argc_reg == 0 && !pp->is_stdcall) {
		fprintf(f, "\tjmp _%s\n\n", sym);
		return;
	}

	fprintf(f, "\tpushl %%edx\n"); // just in case..
	sarg_ofs++;

	// construct arg stack
	stack_args = pp->argc_stack;
	for (i = pp->argc - 1; i >= 0; i--) {
		if (pp->arg[i].reg == NULL) {
			fprintf(f, "\tmovl %d(%%esp), %%edx\n",
				(sarg_ofs + stack_args - 1) * 4);
			fprintf(f, "\tpushl %%edx\n");
			stack_args--;
		}
		else {
			fprintf(f, "\tpushl %%%s\n", pp->arg[i].reg);
		}
		sarg_ofs++;
	}

	// no worries about calling conventions - always __cdecl
	fprintf(f, "\n\tcall _%s\n\n", sym);

	if (sarg_ofs > 2)
		fprintf(f, "\tadd $%d,%%esp\n", (sarg_ofs - 2) * 4);

	fprintf(f, "\tpopl %%edx\n");

	if (pp->is_stdcall && pp->argc_stack)
		fprintf(f, "\tret $%d\n\n", pp->argc_stack * 4);
	else
		fprintf(f, "\tret\n\n");
}

int main(int argc, char *argv[])
{
	FILE *fout, *fsyms_to, *fsyms_from, *fhdr;
	struct parsed_proto pp;
	char line[256];
	char sym[256];
	int ret;

	if (argc != 5) {
		printf("usage:\n%s <bridge.s> <toasm_symf> <fromasm_symf> <hdrf>\n",
			argv[0]);
		return 1;
	}

	hdrfn = argv[4];
	fhdr = fopen(hdrfn, "r");
	my_assert_not(fhdr, NULL);

	fsyms_from = fopen(argv[3], "r");
	my_assert_not(fsyms_from, NULL);

	fsyms_to = fopen(argv[2], "r");
	my_assert_not(fsyms_to, NULL);

	fout = fopen(argv[1], "w");
	my_assert_not(fout, NULL);

	fprintf(fout, ".text\n\n");
	fprintf(fout, "# to asm\n\n");

	while (fgets(line, sizeof(line), fsyms_to))
	{
		next_word(sym, sizeof(sym), line);
		if (sym[0] == 0 || sym[0] == ';' || sym[0] == '#')
			continue;

		ret = proto_parse(fhdr, sym, &pp);
		if (ret)
			goto out;

		out_toasm_x86(fout, sym, &pp);
		proto_release(&pp);
	}

	fprintf(fout, "# from asm\n\n");

	while (fgets(line, sizeof(line), fsyms_from))
	{
		next_word(sym, sizeof(sym), line);
		if (sym[0] == 0 || sym[0] == ';' || sym[0] == '#')
			continue;

		ret = proto_parse(fhdr, sym, &pp);
		if (ret)
			goto out;

		out_fromasm_x86(fout, sym, &pp);
		proto_release(&pp);
	}

	ret = 0;
out:
	fclose(fout);
	fclose(fsyms_to);
	fclose(fsyms_from);
	fclose(fhdr);
	if (ret)
		remove(argv[1]);

	return ret;
}
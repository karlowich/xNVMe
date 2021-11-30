/**
 * Usage Examples
 * --------------
 *
 * ## OS managed AND user-space drivers
 * # Enumerate local system
 * example_enum
 *
 * # Enumerate remote system
 * example_enum 192.168.1.1:4472
 */
#include <libxnvme.h>
#include <libxnvme_pp.h>

int
enum_cb(struct xnvme_dev *dev, void *cb_args)
{
	xnvme_ident_pr(xnvme_dev_get_ident(dev), XNVME_PR_DEF);

	return XNVME_ENUMERATE_DEV_CLOSE;
}

int
main(int argc, char *argv[])
{
	struct xnvme_opts opts = xnvme_opts_default();

	// Enumerate the local or remote system
	// invoking the callback for each discovered device
	xnvme_enumerate(argc > 1 ? argv[1] : NULL, &opts, enum_cb, NULL);

	return 0;
}

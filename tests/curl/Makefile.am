DISTCLEANFILES = *.log *.results oscap_debug.log.*
CLEANFILES = *.log *.results oscap_debug.log.*

TESTS_ENVIRONMENT = \
		builddir=$(top_builddir) \
		OSCAP_FULL_VALIDATION=1 \
		$(top_builddir)/run

TESTS =

if CURLOPT_ACCEPT_ENCODING
TESTS += test_curl_encoding.sh
endif

EXTRA_DIST =	test_curl_encoding.sh \
		ds.xml

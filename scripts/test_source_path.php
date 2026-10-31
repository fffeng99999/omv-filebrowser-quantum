<?php

/**
 * Unit tests for the container path helpers of the FileBrowser Quantum
 * RPC service (openmediavault-filebrowser-quantum 8.1.1).
 *
 * The RPC class normally runs inside omv-engined, so this test provides
 * minimal stubs for the two framework classes it extends/throws and then
 * exercises the private path helpers through reflection. It does not
 * touch the OMV configuration database.
 *
 * Run:  php scripts/test_source_path.php
 */

namespace OMV\Rpc {
	class ServiceAbstract
	{
	}
}

namespace OMV {
	class Exception extends \RuntimeException
	{
	}
}

namespace {
	require __DIR__ . "/../usr/share/openmediavault/engined/rpc/filebrowserquantum.inc";

	$ref = new \ReflectionClass("Engined\\Rpc\\OMVRpcServiceFileBrowserQuantum");
	$service = $ref->newInstanceWithoutConstructor();

	$derive = $ref->getMethod("deriveContainerPath");
	$derive->setAccessible(true);
	$normalize = $ref->getMethod("normalizeContainerPath");
	$normalize->setAccessible(true);
	$validate = $ref->getMethod("validateContainerPath");
	$validate->setAccessible(true);

	$failures = 0;
	$checks = 0;

	function check(string $label, bool $condition, string $detail = ""): void
	{
		global $failures, $checks;
		++$checks;
		if ($condition) {
			echo "ok   - " . $label . PHP_EOL;
			return;
		}
		++$failures;
		echo "FAIL - " . $label . ($detail !== "" ? " (" . $detail . ")" : "") . PHP_EOL;
	}

	function derivePath(string $name, string $uuid = "857c45ee-5fcb-4d2b-8074-de1d66f70a3b"): string
	{
		global $derive, $service;
		return $derive->invoke($service, $name, $uuid);
	}

	function normalizePath(string $path): string
	{
		global $normalize, $service;
		return $normalize->invoke($service, $path);
	}

	function pathIsValid(string $path): bool
	{
		global $validate, $service;
		try {
			$validate->invoke($service, $path);
			return true;
		} catch (\OMV\Exception $e) {
			return false;
		}
	}

	// ---------------------------------------------------------- derivation
	check("simple ASCII name", derivePath("volume0") === "/srv/volume0", derivePath("volume0"));
	check("internal space is sanitized", derivePath("My Photos") === "/srv/My_Photos", derivePath("My Photos"));
	check("Unicode letters are kept", derivePath("照片") === "/srv/照片", derivePath("照片"));
	check("mixed Unicode and space", derivePath("测试 照片") === "/srv/测试_照片", derivePath("测试 照片"));
	check("path separators are sanitized", derivePath("a:b/c") === "/srv/a_b_c", derivePath("a:b/c"));
	check("leading/trailing dots are trimmed", derivePath("...photos...") === "/srv/photos", derivePath("...photos..."));
	check(
		"empty name falls back to the UUID",
		derivePath("", "857c45ee-5fcb-4d2b-8074-de1d66f70a3b") === "/srv/share-857c45ee",
		derivePath("", "857c45ee-5fcb-4d2b-8074-de1d66f70a3b")
	);

	$longName = str_repeat("Long Share Name ", 8);
	$longPath = derivePath($longName);
	check("long name is shortened", strlen($longPath) <= 200, (string)strlen($longPath));
	check("long name is shortened without breaking the path", pathIsValid($longPath), $longPath);
	check(
		"shortened name has a stable hash suffix",
		(bool)preg_match('/^\/srv\/.+-[0-9a-f]{8}$/u', $longPath),
		$longPath
	);

	// ---------------------------------------------------------- normalization
	check("duplicate slashes collapse", normalizePath("/srv//photos///") === "/srv/photos", normalizePath("/srv//photos///"));
	check("whitespace is trimmed", normalizePath("  /srv/photos/  ") === "/srv/photos", normalizePath("  /srv/photos/  "));
	check("empty path stays empty", normalizePath("") === "");
	check("root path is preserved", normalizePath("/") === "/");

	// ------------------------------------------------------------ validation
	check("Unicode container path is accepted", pathIsValid("/srv/照片"));
	check("path with dots/underscores is accepted", pathIsValid("/srv/a_b-c.d"));
	check("relative path is rejected", !pathIsValid("srv/photos"));
	check("path with whitespace is rejected", !pathIsValid("/srv/my photos"));
	check("path with quote is rejected", !pathIsValid("/srv/a\"b"));
	check("path with colon is rejected", !pathIsValid("/srv/a:b"));
	check("dot-dot segment is rejected", !pathIsValid("/srv/../etc"));
	check("dot segment is rejected", !pathIsValid("/srv/./photos"));
	check("root path is rejected", !pathIsValid("/"));
	check("reserved prefix is rejected", !pathIsValid("/proc/1"));
	check("data directory prefix is rejected", !pathIsValid("/home/filebrowser/data/x"));
	check("oversized path is rejected", !pathIsValid("/srv/" . str_repeat("a", 201)));

	echo PHP_EOL . $checks . " checks, " . $failures . " failure(s)" . PHP_EOL;
	exit($failures === 0 ? 0 : 1);
}

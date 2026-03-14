#!/usr/bin/env python3
"""
CiRA CLAW HTTP Tester - Black-box testing via HTTP API
Launches runtime as subprocess, runs test cases, reports results.
"""

import argparse
import subprocess
import time
import sys
import os
import glob
import importlib.util
import requests
from typing import List, Tuple


class RuntimeManager:
    """Manages the runtime subprocess lifecycle"""

    def __init__(self, runtime_path: str, model_path: str, port: int):
        self.runtime_path = runtime_path
        self.model_path = model_path
        self.port = port
        self.process = None
        self.base_url = f"http://localhost:{port}"

    def start(self, timeout: int = 5) -> bool:
        """Launch runtime and wait for it to be ready"""
        print(f"Starting runtime: {self.runtime_path}")
        print(f"  Model: {self.model_path}")
        print(f"  Port: {self.port}")

        try:
            # Launch runtime process
            # Model path is positional argument, -p for port
            # Set cwd to runtime directory so DLLs can be found
            runtime_dir = os.path.dirname(os.path.abspath(self.runtime_path))
            print(f"  Working directory: {runtime_dir}")
            self.process = subprocess.Popen(
                [self.runtime_path, self.model_path, "-p", str(self.port)],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,  # Merge stderr into stdout
                text=True,
                cwd=runtime_dir,
                bufsize=1  # Line buffered
            )

            # Give process a moment to initialize before checking if it died
            time.sleep(0.5)

            # Wait for /health to respond
            start_time = time.time()
            while time.time() - start_time < timeout:
                try:
                    response = requests.get(f"{self.base_url}/health", timeout=0.5)
                    if response.status_code == 200:
                        print(f"[OK] Runtime started successfully")
                        return True
                except requests.RequestException:
                    pass

                # Check if process died
                if self.process.poll() is not None:
                    # Read any output
                    output = self.process.stdout.read() if self.process.stdout else ""
                    print(f"[FAIL] Runtime process died during startup")
                    if output:
                        print(f"  Output: {output}")
                    return False

                time.sleep(0.2)

            print(f"[FAIL] Runtime failed to start within {timeout}s")
            return False

        except Exception as e:
            print(f"[FAIL] Failed to launch runtime: {e}")
            return False

    def stop(self):
        """Stop the runtime process"""
        if self.process:
            print("Stopping runtime...")
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                print("  Force killing runtime...")
                self.process.kill()
                self.process.wait()
            self.process = None

    def is_alive(self) -> bool:
        """Check if runtime is still running"""
        return self.process and self.process.poll() is None


class TestResult:
    """Result of a single test"""

    def __init__(self, name: str, passed: bool, duration: float, error: str = None):
        self.name = name
        self.passed = passed
        self.duration = duration
        self.error = error


class TestRunner:
    """Discovers and runs test cases"""

    def __init__(self, runtime_manager: RuntimeManager):
        self.runtime_manager = runtime_manager
        self.results: List[TestResult] = []

    def discover_tests(self, cases_dir: str) -> List[str]:
        """Find all test_*.py files in cases directory"""
        pattern = os.path.join(cases_dir, "test_*.py")
        test_files = sorted(glob.glob(pattern))
        print(f"\nDiscovered {len(test_files)} test suite(s):")
        for f in test_files:
            print(f"  - {os.path.basename(f)}")
        return test_files

    def load_test_module(self, filepath: str):
        """Dynamically load a test module"""
        module_name = os.path.splitext(os.path.basename(filepath))[0]
        spec = importlib.util.spec_from_file_location(module_name, filepath)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def run_test_suite(self, filepath: str) -> List[TestResult]:
        """Run all tests in a test file"""
        print(f"\n{'='*60}")
        print(f"Running: {os.path.basename(filepath)}")
        print(f"{'='*60}")

        suite_results = []

        try:
            module = self.load_test_module(filepath)

            # Find all test functions (functions starting with 'test_')
            test_functions = [
                getattr(module, name)
                for name in dir(module)
                if name.startswith('test_') and callable(getattr(module, name))
            ]

            if not test_functions:
                print("  No test functions found")
                return suite_results

            # Run each test
            for test_func in test_functions:
                test_name = test_func.__name__

                # Check if runtime crashed
                if not self.runtime_manager.is_alive():
                    result = TestResult(test_name, False, 0.0, "Runtime crashed")
                    suite_results.append(result)
                    print(f"  ERROR {test_name}: Runtime crashed")
                    break

                try:
                    start_time = time.time()
                    test_func(self.runtime_manager.base_url)
                    duration = time.time() - start_time

                    result = TestResult(test_name, True, duration)
                    suite_results.append(result)
                    print(f"  PASS {test_name} ({duration:.3f}s)")

                except AssertionError as e:
                    duration = time.time() - start_time
                    result = TestResult(test_name, False, duration, str(e))
                    suite_results.append(result)
                    print(f"  FAIL {test_name}: {e}")

                except Exception as e:
                    duration = time.time() - start_time
                    result = TestResult(test_name, False, duration, f"Exception: {e}")
                    suite_results.append(result)
                    print(f"  ERROR {test_name}: {e}")

        except Exception as e:
            print(f"  ERROR loading test suite: {e}")

        return suite_results

    def run_all(self, cases_dir: str):
        """Run all discovered test suites"""
        test_files = self.discover_tests(cases_dir)

        if not test_files:
            print("\n[FAIL] No test files found")
            return

        for test_file in test_files:
            suite_results = self.run_test_suite(test_file)
            self.results.extend(suite_results)

    def print_summary(self):
        """Print test results summary"""
        print(f"\n{'='*60}")
        print("SUMMARY")
        print(f"{'='*60}")

        if not self.results:
            print("No tests were run")
            return

        passed = sum(1 for r in self.results if r.passed)
        failed = sum(1 for r in self.results if not r.passed)
        total_time = sum(r.duration for r in self.results)

        print(f"\nTotal tests: {len(self.results)}")
        print(f"  Passed: {passed}")
        print(f"  Failed: {failed}")
        print(f"Total time: {total_time:.3f}s")

        if failed > 0:
            print(f"\nFailed tests:")
            for result in self.results:
                if not result.passed:
                    print(f"  [FAIL] {result.name}: {result.error}")

        print()
        if failed == 0:
            print("[OK] All tests passed!")
            return 0
        else:
            print(f"[FAIL] {failed} test(s) failed")
            return 1


def main():
    parser = argparse.ArgumentParser(description='CiRA CLAW HTTP Tester')
    parser.add_argument('--runtime', required=True, help='Path to test_stream.exe')
    parser.add_argument('--model', required=True, help='Path to model directory')
    parser.add_argument('--port', type=int, default=8089, help='HTTP port (default: 8089)')
    parser.add_argument('--timeout', type=int, default=5, help='Startup timeout in seconds')

    args = parser.parse_args()

    # Validate paths
    if not os.path.exists(args.runtime):
        print(f"[FAIL] Runtime not found: {args.runtime}")
        return 1

    if not os.path.exists(args.model):
        print(f"[FAIL] Model directory not found: {args.model}")
        return 1

    # Create runtime manager
    runtime_mgr = RuntimeManager(args.runtime, args.model, args.port)

    # Start runtime
    if not runtime_mgr.start(timeout=args.timeout):
        return 1

    try:
        # Run tests
        cases_dir = os.path.join(os.path.dirname(__file__), 'cases')
        runner = TestRunner(runtime_mgr)
        runner.run_all(cases_dir)

        # Print summary
        exit_code = runner.print_summary()

        return exit_code

    finally:
        # Always stop runtime
        runtime_mgr.stop()


if __name__ == '__main__':
    sys.exit(main())

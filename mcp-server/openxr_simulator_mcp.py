#!/usr/bin/env python3
"""
OpenXR Simulator MCP Server

This MCP server provides tools for diagnosing OpenXR issues and capturing
screenshots from the OpenXR Simulator runtime.

Features:
- Capture screenshots of current XR frames
- Read and analyze simulator logs
- Get frame timing and diagnostic information
- Monitor session state and head tracking
"""

import asyncio
import base64
import json
import os
import re
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import Any, Optional

# MCP SDK imports
try:
    from mcp.server import Server
    from mcp.server.stdio import stdio_server
    from mcp.types import (
        Tool,
        TextContent,
        ImageContent,
        EmbeddedResource,
    )
except ImportError:
    print("Error: MCP SDK not installed. Run: pip install mcp", file=sys.stderr)
    sys.exit(1)


# Configuration
LOCALAPPDATA = os.environ.get("LOCALAPPDATA", "")
SIMULATOR_DIR = Path(LOCALAPPDATA) / "OpenXR-Simulator"
LOG_FILE = SIMULATOR_DIR / "openxr_simulator.log"
SCREENSHOT_REQUEST_FILE = SIMULATOR_DIR / "screenshot_request.json"
SCREENSHOT_OUTPUT_FILE = SIMULATOR_DIR / "screenshot.bmp"
FRAME_INFO_FILE = SIMULATOR_DIR / "frame_info.json"
STATUS_FILE = SIMULATOR_DIR / "runtime_status.json"

# Create MCP server
server = Server("openxr-simulator")


def ensure_simulator_dir():
    """Ensure the simulator directory exists."""
    SIMULATOR_DIR.mkdir(parents=True, exist_ok=True)


def read_log_file(lines: int = 100, filter_pattern: Optional[str] = None) -> str:
    """Read the last N lines from the simulator log file."""
    if not LOG_FILE.exists():
        return "Log file not found. The OpenXR Simulator may not have been run yet."

    try:
        with open(LOG_FILE, "r", encoding="utf-8", errors="replace") as f:
            all_lines = f.readlines()

        # Get last N lines
        recent_lines = all_lines[-lines:] if len(all_lines) > lines else all_lines

        # Apply filter if specified
        if filter_pattern:
            pattern = re.compile(filter_pattern, re.IGNORECASE)
            recent_lines = [line for line in recent_lines if pattern.search(line)]

        return "".join(recent_lines)
    except Exception as e:
        return f"Error reading log file: {e}"


def parse_log_for_diagnostics() -> dict[str, Any]:
    """Parse the log file to extract diagnostic information."""
    diagnostics = {
        "session_state": "Unknown",
        "frame_count": 0,
        "swapchain_info": [],
        "errors": [],
        "warnings": [],
        "last_activity": None,
        "graphics_api": "Unknown",
        "head_tracking": {"position": None, "orientation": None}
    }

    if not LOG_FILE.exists():
        return diagnostics

    try:
        with open(LOG_FILE, "r", encoding="utf-8", errors="replace") as f:
            content = f.read()

        # Extract session state
        state_matches = re.findall(r"session state -> (\w+)", content)
        if state_matches:
            diagnostics["session_state"] = state_matches[-1]

        # Extract frame count
        frame_matches = re.findall(r"xrEndFrame called \(frame #(\d+)\)", content)
        if frame_matches:
            diagnostics["frame_count"] = int(frame_matches[-1])

        # Extract swapchain info
        swapchain_matches = re.findall(
            r"xrCreateSwapchain.*?format=(\w+).*?(\d+)x(\d+)",
            content
        )
        for match in swapchain_matches:
            diagnostics["swapchain_info"].append({
                "format": match[0],
                "width": int(match[1]),
                "height": int(match[2])
            })

        # Extract errors
        error_matches = re.findall(r"\[SimXR\].*?ERROR.*", content, re.IGNORECASE)
        diagnostics["errors"] = error_matches[-10:]  # Last 10 errors

        # Extract warnings
        warning_matches = re.findall(r"\[SimXR\].*?WARNING.*", content, re.IGNORECASE)
        diagnostics["warnings"] = warning_matches[-10:]  # Last 10 warnings

        # Extract graphics API
        if "D3D12" in content:
            diagnostics["graphics_api"] = "D3D12"
        elif "D3D11" in content:
            diagnostics["graphics_api"] = "D3D11"

        # Get file modification time as last activity
        diagnostics["last_activity"] = datetime.fromtimestamp(
            LOG_FILE.stat().st_mtime
        ).isoformat()

    except Exception as e:
        diagnostics["parse_error"] = str(e)

    return diagnostics


def request_screenshot(eye: str = "both", include_ui: bool = True) -> dict[str, Any]:
    """
    Request a screenshot from the runtime.

    The runtime watches for the screenshot_request.json file and captures
    the frame when it sees it.
    """
    ensure_simulator_dir()

    request = {
        "timestamp": time.time(),
        "eye": eye,  # "left", "right", or "both"
        "include_ui": include_ui,
        "requested_by": "mcp"
    }

    try:
        # Write the request
        with open(SCREENSHOT_REQUEST_FILE, "w") as f:
            json.dump(request, f)

        return {"status": "requested", "request": request}
    except Exception as e:
        return {"status": "error", "error": str(e)}


def wait_for_screenshot(timeout: float = 5.0) -> Optional[bytes]:
    """Wait for the screenshot file to be created."""
    start_time = time.time()

    while time.time() - start_time < timeout:
        if SCREENSHOT_OUTPUT_FILE.exists():
            # Check if file was modified after our request
            try:
                with open(SCREENSHOT_OUTPUT_FILE, "rb") as f:
                    data = f.read()
                # Clean up
                SCREENSHOT_OUTPUT_FILE.unlink(missing_ok=True)
                SCREENSHOT_REQUEST_FILE.unlink(missing_ok=True)
                return data
            except Exception:
                pass
        time.sleep(0.1)

    return None


def get_frame_info() -> dict[str, Any]:
    """Get current frame information from the runtime status file."""
    if STATUS_FILE.exists():
        try:
            with open(STATUS_FILE, "r") as f:
                return json.load(f)
        except Exception as e:
            return {"error": f"Failed to read status: {e}"}

    # Fall back to parsing from log
    return parse_log_for_diagnostics()


def analyze_openxr_issue(symptoms: str) -> dict[str, Any]:
    """Analyze potential OpenXR issues based on symptoms and log data."""
    diagnostics = parse_log_for_diagnostics()
    logs = read_log_file(lines=200)

    analysis = {
        "symptoms": symptoms,
        "diagnostics": diagnostics,
        "potential_issues": [],
        "recommendations": []
    }

    symptoms_lower = symptoms.lower()

    # Check for common issues
    if "black" in symptoms_lower or "blank" in symptoms_lower:
        analysis["potential_issues"].append("Blank/black screen in VR view")
        if diagnostics["frame_count"] == 0:
            analysis["recommendations"].append(
                "No frames have been submitted. Check if xrEndFrame is being called."
            )
        if not diagnostics["swapchain_info"]:
            analysis["recommendations"].append(
                "No swapchains created. Verify xrCreateSwapchain is successful."
            )
        analysis["recommendations"].append(
            "Check that projection layers are being submitted in xrEndFrame."
        )

    if "crash" in symptoms_lower or "error" in symptoms_lower:
        analysis["potential_issues"].append("Runtime errors or crashes")
        if diagnostics["errors"]:
            analysis["recommendations"].append(
                f"Found {len(diagnostics['errors'])} errors in log. Review them below."
            )
            analysis["recent_errors"] = diagnostics["errors"]

    if "tracking" in symptoms_lower or "position" in symptoms_lower:
        analysis["potential_issues"].append("Head tracking issues")
        analysis["recommendations"].append(
            "The simulator uses mouse for head rotation and WASD for movement. "
            "Click the preview window to capture mouse."
        )

    if "performance" in symptoms_lower or "lag" in symptoms_lower or "slow" in symptoms_lower:
        analysis["potential_issues"].append("Performance issues")
        analysis["recommendations"].append(
            "Check frame timing - the simulator targets 90Hz (11.1ms per frame)."
        )
        if diagnostics["graphics_api"] == "D3D12":
            analysis["recommendations"].append(
                "D3D12 path is in use. Complex command list management may cause overhead."
            )

    if "format" in symptoms_lower or "color" in symptoms_lower:
        analysis["potential_issues"].append("Texture format issues")
        if diagnostics["swapchain_info"]:
            analysis["recommendations"].append(
                f"Swapchain formats in use: {[s['format'] for s in diagnostics['swapchain_info']]}"
            )
        analysis["recommendations"].append(
            "Verify sRGB vs linear color space handling is correct."
        )

    # Add log excerpt for context
    if "error" in logs.lower() or "warning" in logs.lower():
        analysis["log_excerpt"] = read_log_file(lines=50, filter_pattern=r"(error|warning|fail)")

    return analysis


# Define MCP tools
@server.list_tools()
async def list_tools() -> list[Tool]:
    """List available tools."""
    return [
        Tool(
            name="capture_screenshot",
            description="Capture a screenshot of the current OpenXR frame being rendered. "
                       "Returns the stereo view (left and right eye) as displayed in the preview window.",
            inputSchema={
                "type": "object",
                "properties": {
                    "eye": {
                        "type": "string",
                        "enum": ["both", "left", "right"],
                        "default": "both",
                        "description": "Which eye view to capture"
                    },
                    "timeout": {
                        "type": "number",
                        "default": 5.0,
                        "description": "Timeout in seconds to wait for screenshot"
                    }
                }
            }
        ),
        Tool(
            name="get_frame_info",
            description="Get detailed information about the current frame including timing, "
                       "resolution, format, head tracking state, and session state.",
            inputSchema={
                "type": "object",
                "properties": {}
            }
        ),
        Tool(
            name="read_logs",
            description="Read recent entries from the OpenXR Simulator log file. "
                       "Useful for debugging issues and understanding runtime behavior.",
            inputSchema={
                "type": "object",
                "properties": {
                    "lines": {
                        "type": "integer",
                        "default": 100,
                        "description": "Number of recent log lines to read"
                    },
                    "filter": {
                        "type": "string",
                        "description": "Optional regex pattern to filter log lines"
                    }
                }
            }
        ),
        Tool(
            name="get_diagnostics",
            description="Get comprehensive diagnostic information about the OpenXR Simulator "
                       "including session state, frame count, swapchain info, and any errors.",
            inputSchema={
                "type": "object",
                "properties": {}
            }
        ),
        Tool(
            name="diagnose_issue",
            description="Analyze potential OpenXR issues based on described symptoms. "
                       "Provides potential causes and recommendations based on log analysis.",
            inputSchema={
                "type": "object",
                "properties": {
                    "symptoms": {
                        "type": "string",
                        "description": "Description of the issue or symptoms you're experiencing"
                    }
                },
                "required": ["symptoms"]
            }
        ),
        Tool(
            name="get_session_state",
            description="Get the current OpenXR session state (IDLE, READY, SYNCHRONIZED, "
                       "VISIBLE, FOCUSED, STOPPING, EXITING).",
            inputSchema={
                "type": "object",
                "properties": {}
            }
        ),
        Tool(
            name="get_head_tracking",
            description="Get the current head tracking state including position and orientation.",
            inputSchema={
                "type": "object",
                "properties": {}
            }
        ),
        Tool(
            name="clear_logs",
            description="Clear the OpenXR Simulator log file to start fresh.",
            inputSchema={
                "type": "object",
                "properties": {}
            }
        )
    ]


@server.call_tool()
async def call_tool(name: str, arguments: dict[str, Any]) -> list[TextContent | ImageContent]:
    """Handle tool calls."""

    if name == "capture_screenshot":
        eye = arguments.get("eye", "both")
        timeout = arguments.get("timeout", 5.0)

        # Request screenshot from runtime
        result = request_screenshot(eye=eye)
        if result["status"] == "error":
            return [TextContent(
                type="text",
                text=f"Failed to request screenshot: {result['error']}"
            )]

        # Wait for screenshot
        screenshot_data = wait_for_screenshot(timeout=timeout)

        if screenshot_data:
            # Return image as base64
            b64_data = base64.standard_b64encode(screenshot_data).decode("utf-8")
            return [
                TextContent(
                    type="text",
                    text=f"Screenshot captured successfully ({len(screenshot_data)} bytes)"
                ),
                ImageContent(
                    type="image",
                    data=b64_data,
                    mimeType="image/bmp"
                )
            ]
        else:
            return [TextContent(
                type="text",
                text="Screenshot timeout. The OpenXR Simulator runtime may not be running, "
                     "or no application is actively rendering frames. Make sure:\n"
                     "1. An OpenXR application is running\n"
                     "2. The OpenXR Simulator is set as the active runtime\n"
                     "3. The application is submitting frames"
            )]

    elif name == "get_frame_info":
        info = get_frame_info()
        return [TextContent(
            type="text",
            text=json.dumps(info, indent=2)
        )]

    elif name == "read_logs":
        lines = arguments.get("lines", 100)
        filter_pattern = arguments.get("filter")
        logs = read_log_file(lines=lines, filter_pattern=filter_pattern)
        return [TextContent(
            type="text",
            text=f"=== OpenXR Simulator Logs (last {lines} lines) ===\n\n{logs}"
        )]

    elif name == "get_diagnostics":
        diagnostics = parse_log_for_diagnostics()
        return [TextContent(
            type="text",
            text=json.dumps(diagnostics, indent=2)
        )]

    elif name == "diagnose_issue":
        symptoms = arguments.get("symptoms", "")
        analysis = analyze_openxr_issue(symptoms)

        output = ["=== OpenXR Issue Analysis ===\n"]
        output.append(f"Symptoms: {analysis['symptoms']}\n")

        if analysis["potential_issues"]:
            output.append("\nPotential Issues:")
            for issue in analysis["potential_issues"]:
                output.append(f"  - {issue}")

        if analysis["recommendations"]:
            output.append("\nRecommendations:")
            for rec in analysis["recommendations"]:
                output.append(f"  - {rec}")

        if "recent_errors" in analysis:
            output.append("\nRecent Errors from Log:")
            for err in analysis["recent_errors"]:
                output.append(f"  {err.strip()}")

        if "log_excerpt" in analysis:
            output.append(f"\nRelevant Log Entries:\n{analysis['log_excerpt']}")

        output.append("\n\nDiagnostics Summary:")
        output.append(json.dumps(analysis["diagnostics"], indent=2))

        return [TextContent(
            type="text",
            text="\n".join(output)
        )]

    elif name == "get_session_state":
        diagnostics = parse_log_for_diagnostics()
        state = diagnostics.get("session_state", "Unknown")

        state_descriptions = {
            "IDLE": "Session created but not yet started",
            "READY": "Session ready to begin rendering",
            "SYNCHRONIZED": "Session synchronized with display",
            "VISIBLE": "Application is visible but not focused",
            "FOCUSED": "Application has focus and full input",
            "STOPPING": "Session is stopping",
            "EXITING": "Session is exiting",
            "Unknown": "State unknown - check if runtime is active"
        }

        return [TextContent(
            type="text",
            text=f"Session State: {state}\n\n{state_descriptions.get(state, '')}"
        )]

    elif name == "get_head_tracking":
        # Read from status file or log
        info = get_frame_info()
        tracking = info.get("head_tracking", {})

        if not tracking or tracking.get("position") is None:
            # Parse from recent logs
            logs = read_log_file(lines=50)
            pos_match = re.search(
                r"head pos.*?(-?\d+\.?\d*),\s*(-?\d+\.?\d*),\s*(-?\d+\.?\d*)",
                logs, re.IGNORECASE
            )
            if pos_match:
                tracking["position"] = {
                    "x": float(pos_match.group(1)),
                    "y": float(pos_match.group(2)),
                    "z": float(pos_match.group(3))
                }

        output = "=== Head Tracking State ===\n\n"
        if tracking.get("position"):
            p = tracking["position"]
            output += f"Position: ({p.get('x', 0):.3f}, {p.get('y', 0):.3f}, {p.get('z', 0):.3f})\n"
        else:
            output += "Position: Default (0.0, 1.7, 0.0)\n"

        if tracking.get("orientation"):
            o = tracking["orientation"]
            output += f"Orientation (quaternion): ({o.get('x', 0):.3f}, {o.get('y', 0):.3f}, {o.get('z', 0):.3f}, {o.get('w', 1):.3f})\n"
        else:
            output += "Orientation: Default (identity)\n"

        output += "\nControls:\n"
        output += "  - Mouse: Look around (click preview window to capture)\n"
        output += "  - WASD: Move forward/left/backward/right\n"
        output += "  - ESC: Release mouse capture\n"

        return [TextContent(
            type="text",
            text=output
        )]

    elif name == "clear_logs":
        try:
            if LOG_FILE.exists():
                LOG_FILE.unlink()
            return [TextContent(
                type="text",
                text="Log file cleared successfully."
            )]
        except Exception as e:
            return [TextContent(
                type="text",
                text=f"Failed to clear log file: {e}"
            )]

    else:
        return [TextContent(
            type="text",
            text=f"Unknown tool: {name}"
        )]


async def main():
    """Run the MCP server."""
    async with stdio_server() as (read_stream, write_stream):
        await server.run(
            read_stream,
            write_stream,
            server.create_initialization_options()
        )


if __name__ == "__main__":
    asyncio.run(main())

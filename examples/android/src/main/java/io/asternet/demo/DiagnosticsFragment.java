package io.asternet.demo;

import android.os.Bundle;
import android.net.ConnectivityManager;
import android.net.NetworkInfo;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.fragment.app.Fragment;

import com.google.android.material.textfield.TextInputEditText;

import io.asternet.AsterNet;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;

public final class DiagnosticsFragment extends Fragment {
    private static final String TAG = "AsterNetDemo";

    private TextInputEditText hostInput;
    private TextInputEditText portInput;
    private Button refreshButton;
    private TextView networkView;
    private TextView qualityView;
    private TextView traceView;
    private TextView diagnosticsView;
    private final AtomicBoolean destroyed = new AtomicBoolean(false);

    @Nullable
    @Override
    public View onCreateView(@NonNull LayoutInflater inflater, @Nullable ViewGroup container,
                             @Nullable Bundle savedInstanceState) {
        View root = inflater.inflate(R.layout.fragment_diagnostics, container, false);
        destroyed.set(false);

        hostInput = root.findViewById(R.id.host_input);
        portInput = root.findViewById(R.id.port_input);
        refreshButton = root.findViewById(R.id.refresh_button);
        networkView = root.findViewById(R.id.network_view);
        qualityView = root.findViewById(R.id.quality_view);
        traceView = root.findViewById(R.id.trace_view);
        diagnosticsView = root.findViewById(R.id.diagnostics_view);

        refreshButton.setOnClickListener(v -> refresh());
        refresh();
        return root;
    }

    private void refresh() {
        if (destroyed.get()) return;
        final String host = hostInput.getText() != null ? hostInput.getText().toString().trim() : "";
        final int port = parseIntSafe(portInput.getText() != null ? portInput.getText().toString().trim() : "443", 443);
        final MainActivity activity = (MainActivity) requireActivity();
        final AsterNet.Client client = activity.getClient();
        if (client == null || host.isEmpty()) {
            traceView.setText("{}");
            diagnosticsView.setText("{}");
            return;
        }

        // 网络类型在主线程读取；真实 traceroute 可能耗时几十秒，放到后台线程避免 ANR。
        final int network = currentNetwork(activity);
        final Object nativeLock = activity.getNativeLock();
        traceView.setText("Trace route\nTracing " + host + ":" + port + " ...");

        new Thread(() -> {
            if (destroyed.get()) return;
            final String trace;
            final String diagnostics;
            synchronized (nativeLock) {
                if (destroyed.get()) return;
                trace = client.traceRoute(host, port);
                diagnostics = client.dumpDiagnostics();
            }

            final String networkText;
            final String qualityText;
            final String traceText;
            final String diagnosticsText;
            try {
                final JSONObject json = new JSONObject(diagnostics);
                final JSONObject quality = json.optJSONObject("quality");
                networkText = "Network state\nAndroid: " + networkName(network)
                    + "\nNetwork epoch: " + json.optLong("network_epoch", -1L);
                qualityText = "Quality snapshot\n"
                    + (quality != null ? quality.toString(2) : "{}");
                traceText = formatTrace(new JSONObject(trace));
                diagnosticsText = "Diagnostics\n" + json.toString(2);
            } catch (Exception error) {
                Log.e(TAG, "Diagnostics refresh failed: " + error.getMessage(), error);
                postResult("<error>", "<error>", "<error>", "<error>");
                return;
            }
            Log.i(TAG, "Diagnostics refresh host=" + host + " port=" + port);
            postResult(networkText, qualityText, traceText, diagnosticsText);
        }, "asternet-diagnostics").start();
    }

    private void postResult(String networkText, String qualityText,
                            String traceText, String diagnosticsText) {
        final View root = getView();
        if (root == null || destroyed.get() || !isAdded()) return;
        root.post(() -> {
            if (destroyed.get() || !isAdded()) return;
            networkView.setText(networkText);
            qualityView.setText(qualityText);
            traceView.setText("Trace route\n" + traceText);
            diagnosticsView.setText(diagnosticsText);
        });
    }

    /** 将 trace route JSON 渲染成逐跳多行文本：`ttl  ip  rtt1  rtt2  rtt3`，超时显示 `*`。 */
    private static String formatTrace(JSONObject json) {
        final String status = json.optString("status", "");
        if ("error".equals(status)) {
            return "Error: " + json.optString("error", "unknown error");
        }

        final String host = json.optString("host", "");
        final String resolvedIp = json.optString("resolved_ip", "");
        final int maxHops = json.optInt("max_hops", 0);
        final int probes = json.optInt("probes_per_hop", 0);

        final StringBuilder sb = new StringBuilder();
        sb.append("traceroute to ").append(host);
        if (!resolvedIp.isEmpty()) sb.append(" (").append(resolvedIp).append(")");
        sb.append(", ").append(maxHops).append(" hops max, ").append(probes).append(" probes/hop");
        if ("reached".equals(status)) sb.append(" [reached]");
        sb.append('\n');

        final JSONArray hops = json.optJSONArray("hops");
        if (hops == null || hops.length() == 0) {
            sb.append("  (no hops)");
            return sb.toString();
        }

        for (int i = 0; i < hops.length(); i++) {
            final JSONObject hop = hops.optJSONObject(i);
            if (hop == null) continue;
            final int ttl = hop.optInt("ttl", 0);
            final String addr = hop.optString("addr", "");
            sb.append(String.format(Locale.US, "%2d  %-15s", ttl, addr.isEmpty() ? "*" : addr));

            final JSONArray rtts = hop.optJSONArray("rtt_ms");
            if (rtts != null) {
                for (int k = 0; k < rtts.length(); k++) {
                    sb.append("   ").append(rttText(rtts.optDouble(k, -1.0)));
                }
            }
            sb.append('\n');
        }
        return sb.toString();
    }

    /** 单条 RTT：负数（超时）显示 `*`，否则显示去掉尾零的毫秒值。 */
    private static String rttText(double ms) {
        if (ms < 0) return "*";
        String value = String.format(Locale.US, "%.2f", ms);
        if (value.contains(".")) {
            while (value.endsWith("0")) value = value.substring(0, value.length() - 1);
            if (value.endsWith(".")) value = value.substring(0, value.length() - 1);
        }
        return value + " ms";
    }

    private static int parseIntSafe(String value, int fallback) {
        try { return Integer.parseInt(value); } catch (NumberFormatException error) { return fallback; }
    }

    private static int currentNetwork(MainActivity activity) {
        ConnectivityManager manager = (ConnectivityManager) activity.getSystemService(android.content.Context.CONNECTIVITY_SERVICE);
        if (manager == null) return AsterNet.Network.UNKNOWN;
        NetworkInfo info = manager.getActiveNetworkInfo();
        if (info == null || !info.isConnected()) return AsterNet.Network.NONE;
        if (info.getType() == ConnectivityManager.TYPE_WIFI) return AsterNet.Network.WIFI;
        if (info.getType() == ConnectivityManager.TYPE_MOBILE) return AsterNet.Network.CELLULAR;
        if (info.getType() == ConnectivityManager.TYPE_ETHERNET) return AsterNet.Network.ETHERNET;
        return AsterNet.Network.UNKNOWN;
    }

    private static String networkName(int network) {
        switch (network) {
            case AsterNet.Network.NONE: return "NONE";
            case AsterNet.Network.WIFI: return "WIFI";
            case AsterNet.Network.CELLULAR: return "CELLULAR";
            case AsterNet.Network.ETHERNET: return "ETHERNET";
            default: return "UNKNOWN";
        }
    }

    @Override
    public void onDestroyView() {
        destroyed.set(true);
        super.onDestroyView();
    }
}

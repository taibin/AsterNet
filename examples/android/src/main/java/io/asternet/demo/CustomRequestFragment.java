package io.asternet.demo;

import android.os.Bundle;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.ProgressBar;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.fragment.app.Fragment;

import com.google.android.material.materialswitch.MaterialSwitch;

import com.google.android.material.card.MaterialCardView;
import com.google.android.material.chip.Chip;
import com.google.android.material.chip.ChipGroup;
import com.google.android.material.snackbar.Snackbar;
import com.google.android.material.textfield.TextInputEditText;

import io.asternet.AsterNet;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

public final class CustomRequestFragment extends Fragment {
    private static final String TAG = "AsterNetDemo";

    private TextInputEditText hostInput;
    private TextInputEditText portInput;
    private TextInputEditText pathInput;
    private ChipGroup methodChipGroup;
    private ChipGroup policyChipGroup;
    private TextInputEditText headersInput;
    private TextInputEditText bodyInput;
    private MaterialCardView bodyCard;
    private TextInputEditText timeoutInput;
    private MaterialSwitch insecureSwitch;
    private Button sendButton;
    private ProgressBar progressBar;
    private TextView resultView;
    private final AtomicBoolean destroyed = new AtomicBoolean(false);

    @Nullable
    @Override
    public View onCreateView(@NonNull LayoutInflater inflater, @Nullable ViewGroup container,
                             @Nullable Bundle savedInstanceState) {
        View root = inflater.inflate(R.layout.fragment_custom_request, container, false);

        hostInput = root.findViewById(R.id.host_input);
        portInput = root.findViewById(R.id.port_input);
        pathInput = root.findViewById(R.id.path_input);
        methodChipGroup = root.findViewById(R.id.method_chip_group);
        policyChipGroup = root.findViewById(R.id.policy_chip_group);
        headersInput = root.findViewById(R.id.headers_input);
        bodyInput = root.findViewById(R.id.body_input);
        bodyCard = root.findViewById(R.id.body_card);
        timeoutInput = root.findViewById(R.id.timeout_input);
        insecureSwitch = root.findViewById(R.id.insecure_switch);
        sendButton = root.findViewById(R.id.send_button);
        progressBar = root.findViewById(R.id.progress_bar);
        resultView = root.findViewById(R.id.result_view);

        if (progressBar != null) progressBar.setVisibility(View.GONE);

        // 根据 method 切换 body 卡片可见性
        methodChipGroup.setOnCheckedStateChangeListener((group, checkedIds) -> {
            if (checkedIds.isEmpty()) return;
            int id = checkedIds.get(0);
            boolean hasBody = id == R.id.chip_post || id == R.id.chip_put;
            bodyCard.setVisibility(hasBody ? View.VISIBLE : View.GONE);
        });

        sendButton.setOnClickListener(v -> submit());

        return root;
    }

    private void submit() {
        if (destroyed.get()) return;

        final String host = hostInput.getText() != null ? hostInput.getText().toString().trim() : "";
        final String portStr = portInput.getText() != null ? portInput.getText().toString().trim() : "443";
        final String path = pathInput.getText() != null ? pathInput.getText().toString().trim() : "/";
        final String timeoutStr = timeoutInput.getText() != null ? timeoutInput.getText().toString().trim() : "12000";

        if (host.isEmpty()) {
            Snackbar.make(resultView, "Host is required", Snackbar.LENGTH_SHORT).show();
            return;
        }

        final int port = parseIntSafe(portStr, 443);
        final int timeoutMs = parseIntSafe(timeoutStr, 12000);

        final String method = getSelectedMethod();
        final int policy = getSelectedPolicy();
        final String headersText = headersInput.getText() != null ? headersInput.getText().toString() : "";
        final String bodyText = bodyInput.getText() != null ? bodyInput.getText().toString() : "";
        final byte[] body = bodyText.getBytes(StandardCharsets.UTF_8);
        final boolean allowInsecure = insecureSwitch != null && insecureSwitch.isChecked();

        final String displayUrl = "https://" + host + (port != 443 ? ":" + port : "") + path;

        sendButton.setEnabled(false);
        if (progressBar != null) progressBar.setVisibility(View.VISIBLE);
        resultView.setText("Running " + method + " " + displayUrl + "\nWaiting for response...");

        Log.i(TAG, "Custom request: " + method + " https://" + host + ":" + port + path
            + " policy=" + AsterNetDemo.policyName(policy) + " timeout=" + timeoutMs);

        new Thread(() -> {
            final long wallStart = System.currentTimeMillis();
            AsterNet.Response response = null;
            try {
                MainActivity activity = (MainActivity) requireActivity();
                synchronized (activity.getNativeLock()) {
                    AsterNet.Client client = activity.getClient();
                    if (destroyed.get() || client == null) return;
                    response = client.request(host, port, method, path, policy,
                        headersText, body, timeoutMs,
                        "GET".equals(method) || "HEAD".equals(method) || "OPTIONS".equals(method),
                        allowInsecure,
                        event -> Log.i(TAG, AsterNetDemo.formatMetrics(event)));
                }
            } catch (Exception e) {
                Log.e(TAG, "Custom request exception: " + e.getMessage(), e);
            }
            final long wallMs = System.currentTimeMillis() - wallStart;
            final AsterNet.Response finalResponse = response;
            if (destroyed.get()) return;
            requireActivity().runOnUiThread(() -> {
                if (destroyed.get()) return;
                if (progressBar != null) progressBar.setVisibility(View.GONE);
                if (finalResponse != null) {
                    Log.i(TAG, "Custom response: result=" + finalResponse.result
                        + " protocol=" + finalResponse.protocolName()
                        + " status=" + finalResponse.status
                        + " bodySize=" + finalResponse.bodySize
                        + " totalMs=" + finalResponse.totalMs);
                    resultView.setText(AsterNetDemo.formatResult("Custom Request", method,
                        displayUrl, policy, finalResponse, wallMs));
                    if (finalResponse.result == -8) {
                        Snackbar.make(resultView, "TLS certificate verification failed", Snackbar.LENGTH_LONG).show();
                    } else if (finalResponse.result == -6 || finalResponse.result == -7) {
                        Snackbar.make(resultView, "DNS/Connection failed — check network", Snackbar.LENGTH_LONG).show();
                    }
                } else {
                    resultView.setText("Request failed: internal exception\nCheck logcat for details.");
                }
                sendButton.setEnabled(true);
            });
        }, "asternet-custom").start();
    }

    private static int parseIntSafe(String s, int fallback) {
        try { return Integer.parseInt(s); } catch (NumberFormatException e) { return fallback; }
    }

    private String getSelectedMethod() {
        int id = methodChipGroup.getCheckedChipId();
        if (id == R.id.chip_post) return "POST";
        if (id == R.id.chip_put) return "PUT";
        if (id == R.id.chip_delete) return "DELETE";
        return "GET";
    }

    private int getSelectedPolicy() {
        int id = policyChipGroup.getCheckedChipId();
        if (id == R.id.chip_h1) return AsterNet.Policy.HTTP_1_1_ONLY;
        if (id == R.id.chip_h2) return AsterNet.Policy.HTTP_2_ONLY;
        if (id == R.id.chip_h3) return AsterNet.Policy.HTTP_3_ONLY;
        return AsterNet.Policy.AUTO;
    }

    @Override
    public void onDestroyView() {
        destroyed.set(true);
        super.onDestroyView();
    }
}

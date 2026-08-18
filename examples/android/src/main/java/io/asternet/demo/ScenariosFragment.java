package io.asternet.demo;

import android.os.Bundle;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.ProgressBar;
import android.widget.Spinner;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.fragment.app.Fragment;

import com.google.android.material.snackbar.Snackbar;

import io.asternet.AsterNet;

import java.net.MalformedURLException;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.atomic.AtomicBoolean;

public final class ScenariosFragment extends Fragment {
    private static final String TAG = "AsterNetDemo";
    private static final int REQUEST_TIMEOUT_MS = 12000;

    private static final Scenario[] SCENARIOS = {
        new Scenario("Automatic GET", "HTTPS GET. Tries HTTP/3, then HTTP/2 and HTTP/1.1.",
            "https://www.cloudflare.com/", "GET", AsterNet.Policy.AUTO, "", "", true),
        new Scenario("HTTP/1.1 GET", "Forces HTTP/1.1 over TLS.",
            "https://www.cloudflare.com/", "GET", AsterNet.Policy.HTTP_1_1_ONLY, "", "", true),
        new Scenario("HTTP/2 GET", "Forces HTTP/2 over TLS.",
            "https://www.cloudflare.com/", "GET", AsterNet.Policy.HTTP_2_ONLY, "", "", true),
        new Scenario("HTTP/3 GET", "Forces HTTP/3 over QUIC.",
            "https://www.cloudflare.com/", "GET", AsterNet.Policy.HTTP_3_ONLY, "", "", true),
        new Scenario("HTTP/2 POST", "Sends a JSON POST.",
            "https://nghttp2.org/httpbin/post", "POST", AsterNet.Policy.HTTP_2_ONLY,
            "content-type: application/json", "{\"source\":\"AsterNet Network Lab\"}", false),
    };

    private Spinner scenarioSpinner;
    private TextView scenarioDetailView;
    private TextView resultView;
    private Button sendButton;
    private ProgressBar progressBar;
    private final AtomicBoolean destroyed = new AtomicBoolean(false);

    @Nullable
    @Override
    public View onCreateView(@NonNull LayoutInflater inflater, @Nullable ViewGroup container,
                             @Nullable Bundle savedInstanceState) {
        View root = inflater.inflate(R.layout.fragment_scenarios, container, false);

        scenarioSpinner = root.findViewById(R.id.scenario_spinner);
        scenarioDetailView = root.findViewById(R.id.scenario_detail_view);
        resultView = root.findViewById(R.id.result_view);
        sendButton = root.findViewById(R.id.send_button);
        progressBar = root.findViewById(R.id.progress_bar);

        String[] labels = new String[SCENARIOS.length];
        for (int i = 0; i < SCENARIOS.length; ++i) labels[i] = SCENARIOS[i].name;
        scenarioSpinner.setAdapter(new ArrayAdapter<>(requireContext(),
            android.R.layout.simple_spinner_dropdown_item, labels));
        scenarioSpinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                showScenario(SCENARIOS[position]);
            }
            @Override
            public void onNothingSelected(AdapterView<?> parent) {}
        });

        showScenario(SCENARIOS[0]);
        if (progressBar != null) progressBar.setVisibility(View.GONE);
        sendButton.setOnClickListener(v -> submit());

        return root;
    }

    private void submit() {
        if (destroyed.get()) return;
        final Scenario scenario = SCENARIOS[scenarioSpinner.getSelectedItemPosition()];
        final ParsedUrl parsed = parseUrl(scenario.url);
        if (parsed == null) return;
        final byte[] body = scenario.body.getBytes(StandardCharsets.UTF_8);

        sendButton.setEnabled(false);
        scenarioSpinner.setEnabled(false);
        if (progressBar != null) progressBar.setVisibility(View.VISIBLE);
        resultView.setText("Running " + scenario.name + "\nWaiting for response...");

        Log.i(TAG, "Request: " + scenario.method + " " + scenario.url + " policy=" + AsterNetDemo.policyName(scenario.policy));

        new Thread(() -> {
            final long wallStart = System.currentTimeMillis();
            AsterNet.Response response = null;
            try {
                MainActivity activity = (MainActivity) requireActivity();
                synchronized (activity.getNativeLock()) {
                    AsterNet.Client client = activity.getClient();
                    if (destroyed.get() || client == null) return;
                    response = client.request(parsed.host, parsed.port,
                        scenario.method, parsed.path, scenario.policy, scenario.headers, body,
                        REQUEST_TIMEOUT_MS, scenario.idempotent,
                        event -> Log.i(TAG, AsterNetDemo.formatMetrics(event)));
                }
            } catch (Exception e) {
                Log.e(TAG, "Request exception: " + e.getMessage(), e);
            }
            final long wallMs = System.currentTimeMillis() - wallStart;
            final AsterNet.Response finalResponse = response;
            if (destroyed.get()) return;
            requireActivity().runOnUiThread(() -> {
                if (destroyed.get()) return;
                if (progressBar != null) progressBar.setVisibility(View.GONE);
                if (finalResponse != null) {
                    Log.i(TAG, "Response: result=" + finalResponse.result
                        + " protocol=" + finalResponse.protocolName()
                        + " status=" + finalResponse.status
                        + " bodySize=" + finalResponse.bodySize
                        + " totalMs=" + finalResponse.totalMs);
                    resultView.setText(AsterNetDemo.formatResult(scenario.name, scenario.method,
                        parsed.displayUrl, scenario.policy, finalResponse, wallMs));
                    if (finalResponse.result == -8) {
                        Snackbar.make(resultView, "TLS certificate verification failed", Snackbar.LENGTH_LONG).show();
                    } else if (finalResponse.result == -6 || finalResponse.result == -7) {
                        Snackbar.make(resultView, "DNS/Connection failed — check network", Snackbar.LENGTH_LONG).show();
                    }
                } else {
                    resultView.setText("Request failed: internal exception\nCheck logcat for details.");
                }
                sendButton.setEnabled(true);
                scenarioSpinner.setEnabled(true);
            });
        }, "asternet-scenarios").start();
    }

    private ParsedUrl parseUrl(String rawUrl) {
        try {
            URL url = new URL(rawUrl);
            if (!"https".equalsIgnoreCase(url.getProtocol())) {
                resultView.setText("AsterNet demo only accepts HTTPS URLs.");
                return null;
            }
            if (url.getHost() == null || url.getHost().isEmpty()) {
                resultView.setText("URL must include a host.");
                return null;
            }
            int port = url.getPort() == -1 ? 443 : url.getPort();
            String path = url.getFile();
            if (path == null || path.isEmpty()) path = "/";
            return new ParsedUrl(url.getHost(), port, path, rawUrl);
        } catch (MalformedURLException error) {
            resultView.setText("Invalid URL: " + error.getMessage());
            return null;
        }
    }

    private void showScenario(Scenario scenario) {
        scenarioDetailView.setText(scenario.description + "\n"
            + scenario.method + " " + scenario.url + "\nPolicy: " + AsterNetDemo.policyName(scenario.policy));
        resultView.setText("AsterNet " + AsterNet.nativeVersion()
            + "\nTLS certificate verification enabled.\n"
            + "Choose a scenario and tap Send request.");
    }

    @Override
    public void onDestroyView() {
        destroyed.set(true);
        super.onDestroyView();
    }

    // -- inner types (shared with CustomRequestFragment via AsterNetDemo) --

    static final class ParsedUrl {
        final String host;
        final int port;
        final String path;
        final String displayUrl;
        ParsedUrl(String host, int port, String path, String displayUrl) {
            this.host = host; this.port = port; this.path = path; this.displayUrl = displayUrl;
        }
    }

    static final class Scenario {
        final String name, description, url, method, headers, body;
        final int policy;
        final boolean idempotent;
        Scenario(String name, String description, String url, String method, int policy,
                 String headers, String body, boolean idempotent) {
            this.name = name; this.description = description; this.url = url;
            this.method = method; this.policy = policy; this.headers = headers;
            this.body = body; this.idempotent = idempotent;
        }
    }
}

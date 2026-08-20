package io.asternet.demo;

import android.os.Bundle;
import android.util.Base64;
import android.util.Log;

import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;
import androidx.fragment.app.Fragment;
import androidx.viewpager2.adapter.FragmentStateAdapter;
import androidx.viewpager2.widget.ViewPager2;

import com.google.android.material.tabs.TabLayout;
import com.google.android.material.tabs.TabLayoutMediator;

import io.asternet.AsterNet;

import java.security.KeyStore;
import java.security.cert.Certificate;
import java.util.Enumeration;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.atomic.AtomicBoolean;

public final class MainActivity extends AppCompatActivity {
    private static final String TAG = "AsterNetDemo";

    private volatile AsterNet.Client client;
    private final Object nativeLock = new Object();
    private final AtomicBoolean destroyed = new AtomicBoolean(false);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        TabLayout tabLayout = findViewById(R.id.tab_layout);
        ViewPager2 viewPager = findViewById(R.id.view_pager);

        viewPager.setAdapter(new FragmentsAdapter(this));

        new TabLayoutMediator(tabLayout, viewPager, (tab, position) -> {
            tab.setText(position == 0 ? "Presets" : position == 1 ? "Custom" : "Diagnostics");
        }).attach();

        Log.i(TAG, "AsterNet Demo starting, native version: " + AsterNet.nativeVersion());

        String caBundle = androidCaBundle();
        String demoCaBundle = demoCaBundle();
        if (!demoCaBundle.isEmpty()) {
            caBundle += demoCaBundle;
            Log.i(TAG, "Demo CA bundle appended: " + demoCaBundle.length() + " chars");
        }
        Log.i(TAG, "CA bundle size: " + caBundle.length() + " chars");
        client = AsterNet.createClient(true, false, caBundle, true);
        Log.i(TAG, "Client created, enableH3=true allowPrivateNetworks=true (demo lab only)");
    }

    /** Called by Fragments to get the shared Client. */
    public AsterNet.Client getClient() {
        return client;
    }

    /** Called by Fragments for thread-safe native access. */
    public Object getNativeLock() {
        return nativeLock;
    }

    private String androidCaBundle() {
        StringBuilder bundle = new StringBuilder();
        int loaded = 0, skipped = 0;
        try {
            KeyStore store = KeyStore.getInstance("AndroidCAStore");
            store.load(null, null);
            Enumeration<String> aliases = store.aliases();
            while (aliases.hasMoreElements()) {
                String alias = aliases.nextElement();
                try {
                    Certificate certificate = store.getCertificate(alias);
                    if (certificate == null || certificate.getEncoded() == null) {
                        skipped++; continue;
                    }
                    bundle.append("-----BEGIN CERTIFICATE-----\n")
                        .append(Base64.encodeToString(certificate.getEncoded(), Base64.NO_WRAP))
                        .append("\n-----END CERTIFICATE-----\n");
                    loaded++;
                } catch (Exception e) {
                    Log.w(TAG, "Skipping CA cert alias=" + alias + " error=" + e.getMessage());
                    skipped++;
                }
            }
        } catch (Exception e) {
            Log.e(TAG, "Failed to load AndroidCAStore: " + e.getMessage(), e);
            return "";
        }
        Log.i(TAG, "CA certs loaded=" + loaded + " skipped=" + skipped);
        if (loaded == 0) Log.w(TAG, "No CA certificates loaded — TLS will fail!");
        return bundle.toString();
    }

    private String demoCaBundle() {
        try (InputStream input = getResources().openRawResource(R.raw.asternet_demo_ca)) {
            ByteArrayOutputStream output = new ByteArrayOutputStream();
            byte[] buffer = new byte[4096];
            for (;;) {
                int read = input.read(buffer);
                if (read < 0) break;
                output.write(buffer, 0, read);
            }
            String pem = output.toString(StandardCharsets.UTF_8.name()).trim();
            if (!pem.contains("-----BEGIN CERTIFICATE-----")) return "";
            return pem + "\n";
        } catch (Exception error) {
            Log.w(TAG, "No demo CA bundle loaded: " + error.getMessage());
            return "";
        }
    }

    @Override
    protected void onDestroy() {
        Log.i(TAG, "onDestroy — closing client");
        destroyed.set(true);
        final AsterNet.Client c = client;
        client = null;
        new Thread(() -> {
            synchronized (nativeLock) {
                if (c != null) c.close();
            }
        }, "asternet-destroy").start();
        super.onDestroy();
    }

    // -- ViewPager2 adapter --

    private static class FragmentsAdapter extends FragmentStateAdapter {
        FragmentsAdapter(@NonNull MainActivity activity) {
            super(activity);
        }

        @NonNull
        @Override
        public Fragment createFragment(int position) {
            if (position == 0) return new ScenariosFragment();
            if (position == 1) return new CustomRequestFragment();
            return new DiagnosticsFragment();
        }

        @Override
        public int getItemCount() {
            return 3;
        }
    }
}

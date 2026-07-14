package com.generals.zh.ui

import android.os.Bundle
import androidx.appcompat.app.AppCompatActivity
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.generals.zh.R
import android.widget.Button

class SettingsActivity : AppCompatActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_settings)

        val backButton = findViewById<Button>(R.id.btn_back)
        backButton.setOnClickListener {
            finish()
        }

        setupSettingsList()
    }

    private fun setupSettingsList() {
        val settingsList = findViewById<RecyclerView>(R.id.settings_list)
        settingsList.layoutManager = LinearLayoutManager(this)
        // Add your settings adapter here
    }
}

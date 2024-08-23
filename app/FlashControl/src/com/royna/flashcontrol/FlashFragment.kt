/*
 * Copyright (C) 2022 The LineageOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.royna.flashcontrol

import android.content.Context
import android.content.ContentResolver
import android.content.SharedPreferences
import android.database.ContentObserver
import android.os.Bundle
import android.os.Looper
import android.os.Handler
import android.os.ServiceManager
import android.provider.Settings
import android.util.Log
import android.widget.Toast
import android.widget.Switch

import androidx.preference.Preference
import androidx.preference.PreferenceFragmentCompat
import androidx.preference.PreferenceManager

import com.android.settingslib.widget.MainSwitchPreference
import com.android.settingslib.widget.OnMainSwitchChangeListener
import com.android.settingslib.widget.RadioButtonPreference

import java.lang.IllegalStateException

import com.royna.flashcontrol.R

import vendor.samsung_ext.hardware.camera.flashlight.IFlashlight

class FlashFragment : PreferenceFragmentCompat(), OnMainSwitchChangeListener {

    private lateinit var switchBar: MainSwitchPreference
    private val mService : IFlashlight? = IFlashlight.Stub.asInterface(ServiceManager.waitForDeclaredService("vendor.samsung_ext.hardware.camera.flashlight.IFlashlight/default"))
    private lateinit var mSharedPreferences : SharedPreferences
    private lateinit var mCurrentIntesity : Preference
    private lateinit var mCurrentOn: Preference

    override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
        addPreferencesFromResource(R.xml.flash_settings)

        mSharedPreferences = PreferenceManager.getDefaultSharedPreferences(requireContext())

        switchBar = findPreference<MainSwitchPreference>(PREF_FLASH_ENABLE)!!
        switchBar.addOnSwitchChangeListener(this)
                val mBrightness = try {
            mService!!.getCurrentBrightness()
        } catch (e : Exception) {
            if (e is NullPointerException) {
                 0
            } else if (e is IllegalStateException) {
                 Log.e(TAG, e.message)
                 0
            } else {
                 throw e // rethrow if it's not one of the expected exceptions
            }
	}
        val mSettingBrightness = Settings.Secure.getInt(requireContext().contentResolver, Settings.Secure.FLASHLIGHT_ENABLED, 0)
        switchBar.isChecked = mBrightness != 0
        switchBar.isEnabled = mSettingBrightness == 0

        val mSavedIntesity = mSharedPreferences.getInt(PREF_FLASH_INTESITY, 1)

        for ((key, value) in PREF_FLASH_MODES) {
            val preference = findPreference<RadioButtonPreference>(key)!!
            preference.isChecked = value == mSavedIntesity
            preference.isEnabled = switchBar.isChecked
            preference.setOnPreferenceClickListener {
                setIntesity(value)
                mSharedPreferences.edit().putInt(PREF_FLASH_INTESITY, value).apply()
                true
            }
        }
        mCurrentOn = findPreference<Preference>(PREF_FLASH_CURRENT_ON)!!
        mCurrentIntesity = findPreference<Preference>(PREF_FLASH_CURRENT_INTESITY)!!
        requireContext().contentResolver.registerContentObserver(mFlashUrl, false, mSettingsObserver)
    }

    private fun changeIntesityView(b: Int) { mCurrentIntesity.title = String.format(requireContext().getString(R.string.flash_current_intesity), b) }
    private fun changeOnOffView(b: Boolean) { mCurrentOn.title = String.format(requireContext().getString(R.string.flash_current_on), requireContext().getString(if (b) R.string.on else R.string.off)) }
    private fun getSettingFlash() = Settings.Secure.getInt(requireContext().contentResolver, Settings.Secure.FLASHLIGHT_ENABLED)

    override fun onResume() {
        super.onResume()
        val mBrightness = mService?.getCurrentBrightness() ?: 0
	changeIntesityView(mBrightness)
	changeOnOffView(mBrightness != 0)
	val isSettingOn = getSettingFlash() != 0
	if (!isSettingOn) {
	    switchBar.apply {
		setChecked(mBrightness != 0)
		isEnabled = mBrightness == 0
	    }
	}
    }

    private val mSettingsObserver = object : ContentObserver(Handler(Looper.getMainLooper())) {
        override fun onChange(selfChange: Boolean) {
            super.onChange(selfChange)
	    if (context == null) return
            try {
		val mMainHandler = Handler(Looper.getMainLooper())
		val mEnabled = getSettingFlash()
                when (mEnabled) {
                    0 -> mMainHandler.post {
		        switchBar.setChecked(false)
			switchBar.isEnabled = true
			changeOnOffView(false)
                    }
                    1 -> mMainHandler.post {
		        switchBar.setChecked(true)
			switchBar.isEnabled = false
		        Toast.makeText(requireContext(), R.string.disabled_qs, Toast.LENGTH_SHORT).show()
		    }
                    else -> return@onChange
		}
		changeRadioButtons(mEnabled == 1)
            } catch (e: Settings.SettingNotFoundException) {
                e.printStackTrace()
            }
        }
    }

    override fun onSwitchChanged(switchView: Switch, isChecked: Boolean) {
        if (mService == null) {
            Log.e(TAG, "mService is null...")
            switchView.setChecked(false)
            return
        }
        try {
            mService.enableFlash(isChecked)
        } catch (e : IllegalStateException) {
            Log.w(TAG, "enableFlash() failed")
            switchView.setChecked(false)
            return
        }
	val kBright = mService.getCurrentBrightness()
        changeOnOffView(isChecked)
        changeIntesityView(kBright)
	setIntesity(kBright)
        changeRadioButtons(isChecked)
    }

    private fun changeRadioButtons(enable: Boolean) {
        for ((key, _) in PREF_FLASH_MODES) {
            val mPreference = findPreference<RadioButtonPreference>(key)!!
            mPreference.isEnabled = enable
        }
    }
            
    private fun setIntesity(intesity: Int) {
        if (intesity < 1 || intesity > 5) {
           Log.e(TAG, "Invalid intesity $intesity")
           return
        }
        if (mService == null) {
           Log.e(TAG, "mService is null...")
           return
        }
        mService.setBrightness(intesity)
        for ((key, value) in PREF_FLASH_MODES) {
            val preference = findPreference<RadioButtonPreference>(key)!!
            preference.isChecked = value == intesity
        }
        mSharedPreferences.edit().putInt(PREF_FLASH_INTESITY, intesity).apply()
        changeIntesityView(mService.getCurrentBrightness())
    }

    override fun onPause() {
        super.onPause()
	beGoneFlash()
    }

    override fun onStop() {
        super.onStop()
        beGoneFlash()
    }

    private fun beGoneFlash() {
	if (getSettingFlash() == 0 && mService?.getCurrentBrightness() ?: 0 != 0) {
	   mService?.enableFlash(false)
	}
    }

    override fun onDestroy() {
	super.onDestroy()
	requireContext().contentResolver.unregisterContentObserver(mSettingsObserver)
    }

    companion object {
        private const val PREF_FLASH_ENABLE = "flash_enable"
        const val PREF_FLASH_INTESITY = "flash_intesity"
        private const val PREF_FLASH_CURRENT_ON = "flash_current_on"
        private const val PREF_FLASH_CURRENT_INTESITY = "flash_current_intesity"
        val PREF_FLASH_MODES = mapOf(
                "flash_intesity_1" to 1,
                "flash_intesity_2" to 2,
                "flash_intesity_3" to 3,
                "flash_intesity_4" to 4,
                "flash_intesity_5" to 5,
        )
        private const val TAG = "FlashCtrl"
        val mFlashUrl = Settings.Secure.getUriFor(Settings.Secure.FLASHLIGHT_ENABLED)
    }
}

#!/usr/bin/python3
# This program is free software; you can redistribute it and/or modify it under
# the terms of the GNU Lesser General Public License as published by the Free
# Software Foundation; either version 3 of the License, or (at your option) any
# later version.  See http://www.gnu.org/copyleft/lgpl.html for the full text
# of the license.

__author__ = 'Marco Trevisan'
__copyright__ = '(c) 2021 Canonical Ltd.'

import os
import subprocess
import sys
import time
import unittest

import dbus
import dbusmock

try:
    import gi
    gi.require_version('AccountsService', '1.0')
    from gi.repository import AccountsService, GLib
    have_accounts_service = True
except (ImportError, ValueError):
    have_accounts_service = False


@unittest.skipUnless(have_accounts_service,
                     'AccountsService gi introspection not available')
class TestAccountsService(dbusmock.DBusTestCase):
    '''Test mocking AccountsService'''

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.start_system_bus()
        cls.dbus_con = cls.get_dbus(True)
        cls.ctx = GLib.main_context_default()

    def setUp(self):
        super().setUp()
        template = os.path.join(
            os.path.dirname(__file__), 'dbusmock/accounts_service.py')
        (self._mock, self._mock_obj) = self.spawn_server_template(
            template, {}, stdout=subprocess.PIPE)
        self._manager = AccountsService.UserManager.get_default()
        while not self._manager.props.is_loaded:
            self.ctx.iteration(True)
        self.assertTrue(self._manager.props.is_loaded)
        self.assertFalse(self._manager.no_service())

    def get_property(self, name):
        return self._mock_obj.Get('org.freedesktop.Accounts', name,
                                  dbus_interface=dbus.PROPERTIES_IFACE)

    def tearDown(self):
        for user in self._manager.list_users():
            self._manager.delete_user(user, False)

        while self._manager.list_users():
            self.ctx.iteration(True)

        if self._mock:
            self._mock.stdout.close()
            self._mock.terminate()
            self._mock.wait()

        while self._manager.props.is_loaded:
            self.ctx.iteration(True)

        self.assertFalse(self._manager.props.is_loaded)
        del self._manager
        super().tearDown()

    def wait_changed(self, user):
        changed = False

        def on_changed(u):
            nonlocal changed
            changed = u is user
        conn_id = user.connect('changed', on_changed)
        while not changed:
            self.ctx.iteration(True)
        user.disconnect(conn_id)

    def test_empty(self):
        self.assertTrue(self._manager.props.is_loaded)
        self.assertFalse(self._manager.list_users())
        self.assertFalse(self._manager.props.has_multiple_users)
        self.assertFalse(self._mock_obj.ListMockUsers())
        self.assertTrue(self.get_property('HasNoUsers'))
        self.assertFalse(self.get_property('HasMultipleUsers'))
        self.assertFalse(self.get_property('AutomaticLoginUsers'))
        self.assertEqual(self.get_property('DaemonVersion'), 'dbus-mock-0.1')

    def test_create_user(self):
        self._manager.create_user(
            'pizza', 'I Love Pizza',
            AccountsService.UserAccountType.ADMINISTRATOR)
        self.assertFalse(self._manager.props.has_multiple_users)
        self.assertFalse(self.get_property('HasNoUsers'))
        [user] = self._manager.list_users()
        self.assertEqual(user.get_account_type(),
                         AccountsService.UserAccountType.ADMINISTRATOR)
        self.assertEqual(user.get_user_name(), 'pizza')
        self.assertEqual(self._manager.get_user(user.get_user_name()), user)
        self.assertEqual(self._manager.get_user_by_id(user.get_uid()), user)

    def test_recreate_user(self):
        self._manager.create_user('pizza', 'I Love Pizza',
            AccountsService.UserAccountType.ADMINISTRATOR)

        with self.assertRaises(GLib.Error) as error:
            self._manager.create_user('pizza', 'I Love Pizza',
                AccountsService.UserAccountType.STANDARD)
        self.assertTrue(error.exception.matches(
            AccountsService.UserManagerError.quark(),
            AccountsService.UserManagerError.FAILED))
        self.assertFalse(self._manager.props.has_multiple_users)

    def test_delete_non_existent_user(self):
        user = self._manager.create_user('not-here-sorry', 'I am leaving...',
            AccountsService.UserAccountType.ADMINISTRATOR)
        self._manager.delete_user(user, False)
        while user in self._manager.list_users():
            self.ctx.iteration(True)

        with self.assertRaises(GLib.Error) as error:
            self._manager.delete_user(user, True)
        self.assertTrue(error.exception.matches(
            AccountsService.UserManagerError.quark(),
            AccountsService.UserManagerError.FAILED))

    def test_manager_signals(self):
        added = None
        removed = None

        def on_added(_manager, u):
            nonlocal added
            added = u

        def on_removed(_manager, u):
            nonlocal removed
            removed = u

        # We only track users if we requested for a list of users
        self.assertFalse(self._manager.list_users())

        self._manager.connect('user-added', on_added)
        self._manager.connect('user-removed', on_removed)
        user = self._manager.create_user('user', 'I am User',
            AccountsService.UserAccountType.ADMINISTRATOR)
        while not added:
            self.ctx.iteration(True)
        self.assertEqual(added, user)
        self.assertIsNone(removed)

        added = None
        user = self._manager.create_user('user2', 'I am User2',
            AccountsService.UserAccountType.STANDARD)
        while not added:
            self.ctx.iteration(True)
        self.assertEqual(added, user)
        self.assertIsNone(removed)

        added = None
        self._manager.delete_user(user, False)

        while not removed:
            self.ctx.iteration(True)
        self.assertEqual(removed, user)
        self.assertIsNone(added)

    def test_create_multiple_users(self):
        self._manager.create_user(
            'pizza', 'I Love Pizza!',
            AccountsService.UserAccountType.ADMINISTRATOR)
        self.assertFalse(self._manager.props.has_multiple_users)
        self.assertFalse(self.get_property('HasNoUsers'))
        [user] = self._manager.list_users()
        self.assertEqual(self._manager.get_user('pizza'), user)
        self.assertEqual(self._manager.get_user_by_id(2001), user)
        self.assertEqual(user.get_account_type(),
                         AccountsService.UserAccountType.ADMINISTRATOR)
        self.assertEqual(user.get_user_name(), 'pizza')
        self.assertEqual(user.get_real_name(), 'I Love Pizza!')

        other = self._manager.create_user(
            'schiacciata', 'I Love Schiacciata too',
            AccountsService.UserAccountType.STANDARD)

        self.assertEqual([user, other], self._manager.list_users())

        while not self._manager.props.has_multiple_users:
            self.ctx.iteration(True)

        self.assertFalse(self.get_property('HasNoUsers'))
        self.assertTrue(self._manager.props.has_multiple_users)
        self.assertIn(other, self._manager.list_users())
        self.assertEqual(self._manager.get_user('schiacciata'), other)
        self.assertEqual(self._manager.get_user_by_id(2002), other)
        self.assertEqual(other.get_uid(), 2002)
        self.assertEqual(other.get_user_name(), 'schiacciata')
        self.assertEqual(other.get_real_name(), 'I Love Schiacciata too')
        self.assertEqual(other.get_account_type(),
                         AccountsService.UserAccountType.STANDARD)

    def test_user_properties_getters(self):
        user = self._manager.create_user(
            'pizza', 'I Love Pizza',
            AccountsService.UserAccountType.ADMINISTRATOR)
        creation_time = int(time.time())

        self.assertEqual(user.get_account_type(),
                         AccountsService.UserAccountType.ADMINISTRATOR)
        self.assertFalse(self._manager.props.has_multiple_users)
        self.assertFalse(user.get_automatic_login())
        self.assertEqual(user.get_email(), 'pizza@python-dbusmock.org')
        self.assertEqual(user.get_home_dir(), '/nonexisting/mock-home/pizza')
        self.assertEqual(user.get_icon_file(), '')
        self.assertEqual(user.get_language(), 'C')
        self.assertEqual(user.get_location(), '')
        self.assertFalse(user.get_locked())
        self.assertEqual(user.get_login_frequency(), 0)
        self.assertEqual(user.get_login_history().unpack(), [])
        self.assertEqual(user.get_login_time(), 0)
        self.assertEqual(user.get_num_sessions(), 0)
        self.assertEqual(user.get_num_sessions_anywhere(), 0)
        self.assertEqual(user.get_object_path(),
                         '/org/freedesktop/Accounts/User2001')
        self.assertEqual(user.get_password_expiration_policy(),
                         (sys.maxsize, creation_time, 0, 0, 0, 0))
        self.assertEqual(user.get_password_hint(), 'Remember it, come on!')
        self.assertEqual(user.get_password_mode(),
                         AccountsService.UserPasswordMode.REGULAR)
        self.assertIsNone(user.get_primary_session_id())
        self.assertEqual(user.get_real_name(), 'I Love Pizza')
        self.assertFalse(user.get_saved())
        self.assertEqual(user.get_session(), 'mock-session')
        self.assertEqual(user.get_session_type(), 'wayland')
        self.assertEqual(user.get_shell(), '/usr/bin/zsh')
        self.assertEqual(user.get_uid(), 2001)
        self.assertEqual(user.get_user_name(), 'pizza')
        self.assertEqual(user.get_x_session(), 'mock-xsession')
        self.assertTrue(user.is_loaded())
        self.assertTrue(user.is_local_account())
        self.assertFalse(user.is_logged_in())
        self.assertFalse(user.is_logged_in_anywhere())
        self.assertFalse(user.is_nonexistent())
        self.assertFalse(user.is_system_account())

    def test_user_properties(self):
        user = self._manager.create_user(
            'pizza', 'I Love Pizza',
            AccountsService.UserAccountType.ADMINISTRATOR)

        self.assertEqual(user.props.account_type,
                         AccountsService.UserAccountType.ADMINISTRATOR)
        self.assertFalse(self._manager.props.has_multiple_users)
        self.assertFalse(user.props.automatic_login)
        self.assertEqual(user.props.email, 'pizza@python-dbusmock.org')
        self.assertEqual(user.props.home_directory,
                         '/nonexisting/mock-home/pizza')
        self.assertEqual(user.props.icon_file, '')
        self.assertEqual(user.props.language, 'C')
        self.assertEqual(user.props.location, '')
        self.assertFalse(user.props.locked)
        self.assertEqual(user.props.login_frequency, 0)
        self.assertEqual(user.props.login_history.unpack(), [])
        self.assertEqual(user.props.login_time, 0)
        self.assertEqual(user.props.password_hint, 'Remember it, come on!')
        self.assertEqual(user.props.password_mode,
                         AccountsService.UserPasswordMode.REGULAR)
        self.assertEqual(user.props.real_name, 'I Love Pizza')
        self.assertEqual(user.props.shell, '/usr/bin/zsh')
        self.assertEqual(user.props.uid, 2001)
        self.assertEqual(user.props.user_name, 'pizza')
        self.assertEqual(user.props.x_session, 'mock-xsession')
        self.assertTrue(user.props.is_loaded)
        self.assertTrue(user.props.local_account)
        self.assertFalse(user.props.nonexistent)

    def test_user_properties_setters(self):
        user = self._manager.create_user(
            'test-user', 'I am a Test user',
            AccountsService.UserAccountType.STANDARD)

        user.set_account_type(
            AccountsService.UserAccountType.ADMINISTRATOR)
        self.wait_changed(user)
        self.assertEqual(user.get_account_type(),
                         AccountsService.UserAccountType.ADMINISTRATOR)

        user.set_account_type(
            AccountsService.UserAccountType.STANDARD)
        self.wait_changed(user)
        self.assertEqual(user.get_account_type(),
                         AccountsService.UserAccountType.STANDARD)

        user.set_automatic_login(True)
        self.wait_changed(user)
        self.assertTrue(user.get_automatic_login())

        user.set_automatic_login(False)
        self.wait_changed(user)
        self.assertFalse(user.get_automatic_login())

        user.set_email('test@email.org')
        self.wait_changed(user)
        self.assertEqual(user.get_email(), 'test@email.org')

        user.set_icon_file('/nonexistant/home/icon.png')
        self.wait_changed(user)
        self.assertEqual(user.get_icon_file(), '/nonexistant/home/icon.png')

        user.set_language('Test Language')
        self.wait_changed(user)
        self.assertEqual(user.get_language(), 'Test Language')

        user.set_location('Test Location')
        self.wait_changed(user)
        self.assertEqual(user.get_location(), 'Test Location')

        user.set_locked(True)
        self.wait_changed(user)
        self.assertTrue(user.get_locked())

        user.set_locked(False)
        self.wait_changed(user)
        self.assertFalse(user.get_locked())

        user.set_password('Test Password', 'Test PasswordHint')
        self.wait_changed(user)
        self.assertEqual(user.get_password_hint(), 'Test PasswordHint')

        user.set_password_hint('Another Test Password Hint')
        self.wait_changed(user)
        self.assertEqual(user.get_password_hint(),
                         'Another Test Password Hint')

        user.set_password_mode(AccountsService.UserPasswordMode.NONE)
        self.wait_changed(user)
        self.assertEqual(user.get_password_mode(),
                         AccountsService.UserPasswordMode.NONE)

        user.set_password_mode(AccountsService.UserPasswordMode.REGULAR)
        self.wait_changed(user)
        self.assertEqual(user.get_password_mode(),
                         AccountsService.UserPasswordMode.REGULAR)

        user.set_password_mode(AccountsService.UserPasswordMode.SET_AT_LOGIN)
        self.wait_changed(user)
        self.assertEqual(user.get_password_mode(),
                         AccountsService.UserPasswordMode.SET_AT_LOGIN)

        user.set_real_name('Test RealName')
        self.wait_changed(user)
        self.assertEqual(user.get_real_name(), 'Test RealName')

        user.set_session('Test Session')
        self.wait_changed(user)
        self.assertEqual(user.get_session(), 'Test Session')

        user.set_session_type('Test SessionType')
        self.wait_changed(user)
        self.assertEqual(user.get_session_type(), 'Test SessionType')

        user.set_user_name('new-test-user')
        self.wait_changed(user)
        self.assertEqual(user.get_user_name(), 'new-test-user')

        user.set_x_session('Test XSession')
        self.wait_changed(user)
        self.assertEqual(user.get_x_session(), 'Test XSession')

    def test_cache_user_errors(self):
        with self.assertRaises(GLib.Error) as error:
            self.assertFalse(self._manager.get_user('not-here'))
        self.assertTrue(error.exception.matches(
            AccountsService.UserManagerError.quark(),
            AccountsService.UserManagerError.FAILED))

        with self.assertRaises(GLib.Error) as error:
            self.assertFalse(self._manager.get_user_by_id(123456))
        self.assertTrue(error.exception.matches(
            AccountsService.UserManagerError.quark(),
            AccountsService.UserManagerError.FAILED))

    def test_cache_user(self):
        user = self._manager.create_user('user', 'I am User',
            AccountsService.UserAccountType.ADMINISTRATOR)
        self.assertEqual(self._manager.cache_user('user'), user)
        self.assertTrue(self._manager.uncache_user('user'))

    def test_cache_user_errors(self):
        with self.assertRaises(GLib.Error) as error:
            self.assertFalse(self._manager.cache_user('not-here'))
        self.assertTrue(error.exception.matches(
            AccountsService.UserManagerError.quark(),
            AccountsService.UserManagerError.FAILED))

        with self.assertRaises(GLib.Error) as error:
            self.assertFalse(self._manager.uncache_user('not-here'))
        self.assertTrue(error.exception.matches(
            AccountsService.UserManagerError.quark(),
            AccountsService.UserManagerError.FAILED))

    def test_automatic_login_users(self):
        user = self._manager.create_user(
            'test-user', 'I am a Test user',
            AccountsService.UserAccountType.STANDARD)

        user.set_automatic_login(True)
        self.wait_changed(user)
        self.assertTrue(user.get_automatic_login())
        self.assertEqual(
            [user.get_object_path()],
            self.get_property('AutomaticLoginUsers'))
        self.assertCountEqual(self._mock_obj.ListMockUsers(),
                              self.get_property('AutomaticLoginUsers'))

        user2 = self._manager.create_user(
            'another-test-user', 'I am another Test user',
            AccountsService.UserAccountType.STANDARD)
        self.assertNotIn(user2.get_object_path(),
                         self.get_property('AutomaticLoginUsers'))

        user2.set_automatic_login(True)
        self.assertIn(user2.get_object_path(),
                      self.get_property('AutomaticLoginUsers'))
        self.assertEqual(len(self.get_property('AutomaticLoginUsers')), 2)
        self.assertCountEqual(self._mock_obj.ListMockUsers(),
                              self.get_property('AutomaticLoginUsers'))

        user.set_automatic_login(False)
        self.wait_changed(user)
        self.assertFalse(user.get_automatic_login())
        self.assertNotIn(user.get_object_path(),
                         self.get_property('AutomaticLoginUsers'))
        self.assertEqual(len(self.get_property('AutomaticLoginUsers')), 1)

        self._manager.delete_user(user2, False)
        while user2 in self._manager.list_users():
            self.ctx.iteration(True)
        self.assertFalse(self.get_property('AutomaticLoginUsers'))


if __name__ == '__main__':
    # avoid writing to stderr
    run = unittest.main(testRunner=unittest.TextTestRunner(
        stream=sys.stdout, verbosity=2))

    if run.result.errors or run.result.failures:
        sys.exit(1)

    # Translate to skip error
    if run.result.testsRun == len(run.result.skipped):
        sys.exit(77)
